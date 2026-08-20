// Account-level custom fonts. Solves two problems that OS/browser-level fonts
// can't: the live editor's font picker (main.js getAvailableFonts) depends on
// the browser's Local Font Access API, which only exists in a secure context
// (HTTPS, or literally "localhost") and only in Chromium — so it silently
// falls back to a generic list on any plain-HTTP deployment. And PDF export
// (export.rs) renders in a headless-Chromium sandbox that blocks every
// network request except file:// and data: (deliberately, to close off SSRF)
// — so a font installed on the visitor's machine, or even the server's own
// OS, can never reach that renderer by name alone.
//
// Fonts uploaded here sidestep both: they're served as real HTTP assets for
// @font-face in the live app (works in any browser, any origin), and
// base64-embedded directly into the exported HTML's <style> for PDF export
// (satisfies the sandbox's data:-only policy with no loosening required).

use axum::body::Bytes;
use axum::extract::{Path as AxumPath, Query, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::Json;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use tower_sessions::Session;
use uuid::Uuid;

use crate::auth::current_user_id_with_headers;
use crate::error::AppError;
use crate::state::AppState;

pub const MAX_FONT_BYTES: usize = 5 * 1024 * 1024; // 5MB per font file
const MAX_FONTS_PER_ACCOUNT: i64 = 30;

fn content_type_for(ext: &str) -> Option<&'static str> {
    match ext.to_lowercase().as_str() {
        "ttf" => Some("font/ttf"),
        "otf" => Some("font/otf"),
        "woff" => Some("font/woff"),
        "woff2" => Some("font/woff2"),
        _ => None,
    }
}

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn font_file_path(state: &AppState, owner_id: &str, font_id: &str, ext: &str) -> PathBuf {
    state.fonts_dir.join(owner_id).join(format!("{font_id}.{ext}"))
}

#[derive(Serialize, sqlx::FromRow)]
pub struct FontView {
    id: String,
    #[serde(rename = "familyName")]
    family_name: String,
    #[serde(rename = "contentType")]
    content_type: String,
    #[serde(rename = "byteSize")]
    byte_size: i64,
}

pub async fn list_fonts(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
) -> Result<Json<Vec<FontView>>, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let rows = sqlx::query_as::<_, FontView>(
        "SELECT id, family_name, content_type, byte_size FROM custom_fonts
         WHERE owner_id = ? ORDER BY family_name",
    )
    .bind(&user_id)
    .fetch_all(&state.db)
    .await?;
    Ok(Json(rows))
}

#[derive(Deserialize)]
pub struct UploadFontQuery {
    #[serde(rename = "familyName")]
    family_name: String,
    filename: String,
}

pub async fn upload_font(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<UploadFontQuery>,
    body: Bytes,
) -> Result<Json<FontView>, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;

    let family_name = q.family_name.trim();
    // The name is interpolated into a CSS string inside a `<style>` element
    // for PDF export (see embed_account_fonts_style); reject anything that
    // could escape either context up front, so what's stored is always safe
    // to emit verbatim.
    if family_name.is_empty()
        || family_name.len() > 100
        || family_name.chars().any(|c| {
            c.is_control() || matches!(c, '\'' | '"' | '<' | '>' | '\\' | '{' | '}' | ';' | '&')
        })
    {
        return Err(AppError::BadRequest("invalid font family name".to_string()));
    }
    let ext = std::path::Path::new(&q.filename)
        .extension()
        .and_then(|e| e.to_str())
        .unwrap_or("")
        .to_lowercase();
    let content_type = content_type_for(&ext)
        .ok_or_else(|| AppError::BadRequest("font must be .ttf, .otf, .woff or .woff2".to_string()))?;

    if body.is_empty() || body.len() > MAX_FONT_BYTES {
        return Err(AppError::BadRequest("font file too large (5MB max)".to_string()));
    }

    let count = sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM custom_fonts WHERE owner_id = ?")
        .bind(&user_id)
        .fetch_one(&state.db)
        .await?;
    if count >= MAX_FONTS_PER_ACCOUNT {
        return Err(AppError::BadRequest(format!(
            "font library is full ({MAX_FONTS_PER_ACCOUNT} max) — delete one first"
        )));
    }

    let id = Uuid::new_v4().to_string();
    let path = font_file_path(&state, &user_id, &id, &ext);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(&path, &body)?;

    // Re-uploading an existing family name replaces the file; the UNIQUE
    // constraint means the insert would otherwise fail outright.
    let existing = sqlx::query_as::<_, (String, String)>(
        "SELECT id, content_type FROM custom_fonts WHERE owner_id = ? AND family_name = ?",
    )
    .bind(&user_id)
    .bind(family_name)
    .fetch_optional(&state.db)
    .await?;

    if let Some((old_id, old_ct)) = existing {
        let old_ext = old_ct.rsplit('/').next().unwrap_or("");
        let _ = std::fs::remove_file(font_file_path(&state, &user_id, &old_id, old_ext));
        sqlx::query("DELETE FROM custom_fonts WHERE id = ?")
            .bind(&old_id)
            .execute(&state.db)
            .await?;
    }

    sqlx::query(
        "INSERT INTO custom_fonts (id, owner_id, family_name, filename, content_type, byte_size, created_at)
         VALUES (?, ?, ?, ?, ?, ?, ?)",
    )
    .bind(&id)
    .bind(&user_id)
    .bind(family_name)
    .bind(&q.filename)
    .bind(content_type)
    .bind(body.len() as i64)
    .bind(now_ms())
    .execute(&state.db)
    .await?;

    Ok(Json(FontView {
        id,
        family_name: family_name.to_string(),
        content_type: content_type.to_string(),
        byte_size: body.len() as i64,
    }))
}

#[derive(sqlx::FromRow)]
struct FontRow {
    owner_id: String,
    content_type: String,
}

async fn load_font_row(state: &AppState, font_id: &str) -> Result<FontRow, AppError> {
    sqlx::query_as::<_, FontRow>("SELECT owner_id, content_type FROM custom_fonts WHERE id = ?")
        .bind(font_id)
        .fetch_optional(&state.db)
        .await?
        .ok_or(AppError::NotFound)
}

pub async fn font_file(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    AxumPath(font_id): AxumPath<String>,
) -> Result<axum::response::Response, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let row = load_font_row(&state, &font_id).await?;
    if row.owner_id != user_id {
        return Err(AppError::NotFound);
    }
    let ext = row.content_type.rsplit('/').next().unwrap_or("ttf");
    let bytes = std::fs::read(font_file_path(&state, &user_id, &font_id, ext))?;
    Ok((
        StatusCode::OK,
        [
            (header::CONTENT_TYPE, row.content_type),
            // The font list rarely changes; the app re-injects @font-face on
            // every load anyway, so a modest cache just saves repeat fetches.
            (header::CACHE_CONTROL, "private, max-age=3600".to_string()),
        ],
        bytes,
    )
        .into_response())
}

pub async fn delete_font(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    AxumPath(font_id): AxumPath<String>,
) -> Result<StatusCode, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let row = load_font_row(&state, &font_id).await?;
    if row.owner_id != user_id {
        return Err(AppError::NotFound);
    }
    let ext = row.content_type.rsplit('/').next().unwrap_or("ttf");
    let _ = std::fs::remove_file(font_file_path(&state, &user_id, &font_id, ext));
    sqlx::query("DELETE FROM custom_fonts WHERE id = ?")
        .bind(&font_id)
        .execute(&state.db)
        .await?;
    Ok(StatusCode::OK)
}

/// Sanitizes a user-supplied font family name for use inside a single-quoted
/// CSS string that itself sits inside an HTML `<style>` element. Dropping
/// only `'` isn't enough: `<` would let a family name close the style element
/// early (`</style><script>…`), and a raw newline or backslash can break out
/// of the CSS string. Keep it to characters that can only ever be text.
fn css_string_literal_body(family_name: &str) -> String {
    family_name
        .chars()
        .filter(|c| !matches!(c, '\'' | '"' | '<' | '>' | '\\' | '{' | '}' | ';' | '&'))
        .filter(|c| !c.is_control())
        .collect()
}

/// Builds a `<style>` block declaring every one of the user's custom fonts as
/// `@font-face` with the file base64-embedded as a `data:` URI. Used by
/// export.rs, whose headless-Chromium sandbox refuses any request that isn't
/// `file://` or `data:` — an HTTP `url()` back to this server would simply be
/// dropped, so the bytes have to already be inline before Chromium sees them.
pub async fn embed_account_fonts_style(state: &AppState, owner_id: &str) -> String {
    let rows = sqlx::query_as::<_, FontView>(
        "SELECT id, family_name, content_type, byte_size FROM custom_fonts WHERE owner_id = ?",
    )
    .bind(owner_id)
    .fetch_all(&state.db)
    .await
    .unwrap_or_default();

    if rows.is_empty() {
        return String::new();
    }

    let mut style = String::from("<style>\n");
    for row in rows {
        let ext = row.content_type.rsplit('/').next().unwrap_or("ttf");
        let path = font_file_path(state, owner_id, &row.id, ext);
        let Ok(bytes) = std::fs::read(&path) else { continue };
        let b64 = base64_encode(&bytes);
        let format = match ext {
            "woff2" => "woff2",
            "woff" => "woff",
            "otf" => "opentype",
            _ => "truetype",
        };
        style.push_str(&format!(
            "@font-face{{font-family:'{}';src:url(data:{};base64,{}) format('{}');}}\n",
            css_string_literal_body(&row.family_name),
            row.content_type,
            b64,
            format,
        ));
    }
    style.push_str("</style>\n");
    style
}

// A tiny std-only base64 encoder so this module doesn't need a new
// dependency just for embedding a handful of font files per export.
fn base64_encode(data: &[u8]) -> String {
    const CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0];
        let b1 = *chunk.get(1).unwrap_or(&0);
        let b2 = *chunk.get(2).unwrap_or(&0);
        out.push(CHARS[(b0 >> 2) as usize] as char);
        out.push(CHARS[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
        out.push(if chunk.len() > 1 { CHARS[(((b1 & 0x0f) << 2) | (b2 >> 6)) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { CHARS[(b2 & 0x3f) as usize] as char } else { '=' });
    }
    out
}

use axum::response::IntoResponse;

#[cfg(test)]
mod tests {
    use super::base64_encode;

    // RFC 4648 §10 test vectors.
    #[test]
    fn base64_matches_rfc4648_vectors() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foob"), "Zm9vYg==");
        assert_eq!(base64_encode(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn base64_roundtrips_binary_font_bytes() {
        // Font files are arbitrary binary, not text — exercise every byte
        // value including 0x00 and 0xFF to catch any sign-extension bug in
        // the bit-shifting.
        let data: Vec<u8> = (0..=255u8).collect();
        let encoded = base64_encode(&data);
        // Decode it back with a tiny inline decoder and compare, so this
        // test doesn't depend on a base64 crate existing.
        let decoded = decode_for_test(&encoded);
        assert_eq!(decoded, data);
    }

    fn decode_for_test(s: &str) -> Vec<u8> {
        fn val(c: u8) -> u32 {
            match c {
                b'A'..=b'Z' => (c - b'A') as u32,
                b'a'..=b'z' => (c - b'a' + 26) as u32,
                b'0'..=b'9' => (c - b'0' + 52) as u32,
                b'+' => 62,
                b'/' => 63,
                _ => 0,
            }
        }
        let bytes = s.as_bytes();
        let mut out = Vec::new();
        for chunk in bytes.chunks(4) {
            let pad = chunk.iter().filter(|&&c| c == b'=').count();
            let n = val(chunk[0]) << 18
                | val(chunk[1]) << 12
                | val(*chunk.get(2).unwrap_or(&b'A')) << 6
                | val(*chunk.get(3).unwrap_or(&b'A'));
            out.push((n >> 16) as u8);
            if pad < 2 {
                out.push((n >> 8) as u8);
            }
            if pad < 1 {
                out.push(n as u8);
            }
        }
        out
    }
}
