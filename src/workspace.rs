// Workspace file CRUD, ported from the Tauri backend (src-tauri/src/lib.rs) in the
// original OfficeSuite desktop app. There, `root` came straight from the client
// because it was gated by an OS folder-picker dialog for a single local user. On
// the web there is no such gate, so `root` is instead always derived from the
// authenticated session (see `current_user_id`), and every relative path is
// validated with `safe_rel_path` to make sure it can't escape that root.

use axum::extract::{Query, State};
use axum::{http::HeaderMap, http::StatusCode, Json};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::time::UNIX_EPOCH;
use tower_sessions::Session;

use crate::auth::current_user_id_with_headers;
use crate::error::AppError;
use crate::state::AppState;

#[derive(Serialize)]
pub struct FsEntry {
    name: String,
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    modified: u64,
    /// sha256 hex digest of the file's bytes, empty string for directories.
    /// Lets sync clients tell "did this change on the server" from one
    /// listing call without downloading every file's content.
    hash: String,
    children: Vec<FsEntry>,
}

fn hash_file(path: &Path) -> String {
    match fs::read(path) {
        Ok(bytes) => {
            let mut hasher = Sha256::new();
            hasher.update(&bytes);
            format!("{:x}", hasher.finalize())
        }
        Err(_) => String::new(),
    }
}

pub const WORK_FILE_EXTENSIONS: [&str; 8] = ["mdp", "mds", "mdg", "mdn", "mdl", "mdc", "mde", "mdb"];

/// Root-level sidecar files that sync clients need to see in listings (with a
/// hash) even though they aren't work files. Currently just the user's custom
/// document templates, so templates follow their account across devices. The
/// web UI filters these out of its file tree; the desktop tree never lists
/// them (its walker only shows work extensions).
const SYNC_SIDECAR_FILES: [&str; 1] = ["_lktpl.json"];

fn modified_ms(metadata: &fs::Metadata) -> u64 {
    metadata
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

/// Rejects absolute paths, drive-letter prefixes, and `..` components so a
/// request can never resolve to a path outside the caller's workspace root.
pub fn safe_rel_path(rel_path: &str) -> Result<PathBuf, AppError> {
    let mut buf = PathBuf::new();
    for component in Path::new(rel_path).components() {
        match component {
            Component::Normal(seg) => buf.push(seg),
            Component::CurDir => {}
            _ => return Err(AppError::BadRequest("invalid path".to_string())),
        }
    }
    if buf.as_os_str().is_empty() {
        return Err(AppError::BadRequest("invalid path".to_string()));
    }
    Ok(buf)
}

async fn user_root(
    state: &AppState,
    session: &Session,
    headers: &HeaderMap,
) -> Result<PathBuf, AppError> {
    let user_id = current_user_id_with_headers(state, session, headers).await?;
    let root = state.workspaces_dir.join(user_id);
    fs::create_dir_all(&root)?;
    Ok(root)
}

pub fn walk_work_dir(dir: &Path, rel_prefix: &str) -> Result<Vec<FsEntry>, AppError> {
    let mut entries = Vec::new();

    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();
        let metadata = entry.metadata()?;

        let rel_path = if rel_prefix.is_empty() {
            name.clone()
        } else {
            format!("{}/{}", rel_prefix, name)
        };

        if metadata.is_dir() {
            entries.push(FsEntry {
                name,
                children: walk_work_dir(&path, &rel_path)?,
                rel_path,
                is_dir: true,
                modified: 0,
                hash: String::new(),
            });
        } else {
            let ext = Path::new(&name)
                .extension()
                .and_then(|e| e.to_str())
                .map(|e| e.to_lowercase())
                .unwrap_or_default();

            let is_root_sidecar =
                rel_prefix.is_empty() && SYNC_SIDECAR_FILES.contains(&name.as_str());
            if !WORK_FILE_EXTENSIONS.contains(&ext.as_str()) && !is_root_sidecar {
                continue;
            }

            entries.push(FsEntry {
                name,
                rel_path,
                is_dir: false,
                modified: modified_ms(&metadata),
                hash: hash_file(&path),
                children: Vec::new(),
            });
        }
    }

    entries.sort_by(|a, b| match (a.is_dir, b.is_dir) {
        (true, false) => std::cmp::Ordering::Less,
        (false, true) => std::cmp::Ordering::Greater,
        _ => a.name.to_lowercase().cmp(&b.name.to_lowercase()),
    });

    Ok(entries)
}

pub async fn list_work_folder(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
) -> Result<Json<Vec<FsEntry>>, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    Ok(Json(walk_work_dir(&root, "")?))
}

#[derive(Deserialize)]
pub struct PathQuery {
    path: String,
}

pub async fn read_work_file(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<PathQuery>,
) -> Result<String, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    let rel = safe_rel_path(&q.path)?;
    fs::read_to_string(root.join(rel)).map_err(|e| e.into())
}

pub async fn write_work_file(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<PathQuery>,
    content: String,
) -> Result<StatusCode, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    let rel = safe_rel_path(&q.path)?;
    let path = root.join(rel);

    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::write(path, content)?;
    Ok(StatusCode::OK)
}

#[derive(Deserialize)]
pub struct RelPathBody {
    #[serde(rename = "relPath")]
    rel_path: String,
}

pub async fn create_work_folder(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Json(body): Json<RelPathBody>,
) -> Result<StatusCode, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    let rel = safe_rel_path(&body.rel_path)?;
    fs::create_dir_all(root.join(rel))?;
    Ok(StatusCode::OK)
}

#[derive(Deserialize)]
pub struct DeleteQuery {
    path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
}

pub async fn delete_work_entry(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<DeleteQuery>,
) -> Result<StatusCode, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    let rel = safe_rel_path(&q.path)?;
    let path = root.join(rel);

    if q.is_dir {
        fs::remove_dir_all(path)?;
    } else {
        fs::remove_file(path)?;
    }
    Ok(StatusCode::OK)
}

#[derive(Deserialize)]
pub struct MoveBody {
    from: String,
    to: String,
}

pub async fn move_work_entry(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Json(body): Json<MoveBody>,
) -> Result<StatusCode, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    let from = root.join(safe_rel_path(&body.from)?);
    let to = root.join(safe_rel_path(&body.to)?);

    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::rename(from, to)?;
    Ok(StatusCode::OK)
}
