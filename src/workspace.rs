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
/// hash) even though they aren't work files: the user's custom document
/// templates and roaming UI preferences (theme), so both follow the account
/// across devices. The web UI filters these out of its file tree; the desktop
/// tree never lists them (its walker only shows work extensions).
const SYNC_SIDECAR_FILES: [&str; 2] = ["_lktpl.json", "_lkprefs.json"];

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

/// Total bytes stored under a workspace root. Walks the tree rather than
/// tracking a running total in the DB: workspaces are small (text files) and
/// this way the number can't drift out of sync with what's on disk.
fn dir_size_bytes(dir: &Path) -> u64 {
    let Ok(read) = fs::read_dir(dir) else { return 0 };
    read.flatten()
        .map(|entry| match entry.metadata() {
            Ok(m) if m.is_dir() => dir_size_bytes(&entry.path()),
            Ok(m) => m.len(),
            Err(_) => 0,
        })
        .sum()
}

/// Registration is open, so an unbounded workspace lets any account fill the
/// server's disk. Checked before a write that would grow the workspace;
/// replacing a file with a smaller one always succeeds, so a user who hits
/// the ceiling can still edit their way back under it.
pub fn check_quota(
    state: &AppState,
    root: &Path,
    target: &Path,
    incoming_len: u64,
) -> Result<(), AppError> {
    if state.workspace_quota_bytes == 0 {
        return Ok(());
    }
    let existing_len = fs::metadata(target).map(|m| m.len()).unwrap_or(0);
    if incoming_len <= existing_len {
        return Ok(());
    }
    let projected = dir_size_bytes(root) - existing_len + incoming_len;
    if projected > state.workspace_quota_bytes {
        return Err(AppError::BadRequest(format!(
            "workspace is full ({} MB max) — delete something first",
            state.workspace_quota_bytes / (1024 * 1024)
        )));
    }
    Ok(())
}

async fn user_ctx(
    state: &AppState,
    session: &Session,
    headers: &HeaderMap,
) -> Result<(String, PathBuf), AppError> {
    let user_id = current_user_id_with_headers(state, session, headers).await?;
    let root = state.workspaces_dir.join(&user_id);
    fs::create_dir_all(&root)?;
    Ok((user_id, root))
}

async fn user_root(
    state: &AppState,
    session: &Session,
    headers: &HeaderMap,
) -> Result<PathBuf, AppError> {
    Ok(user_ctx(state, session, headers).await?.1)
}

// ── Deletion tombstones (see migrations/0007_deleted_files.sql) ──

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// Canonical tombstone key for a validated rel path: forward-slash separators,
/// matching the rel-path form clients use in every request and listing.
fn rel_key(rel: &Path) -> String {
    rel.components()
        .map(|c| c.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/")
}

async fn record_tombstone(state: &AppState, user_id: &str, rel_path: &str) {
    let _ = sqlx::query(
        "INSERT INTO deleted_files (user_id, rel_path, deleted_at) VALUES (?, ?, ?)
         ON CONFLICT(user_id, rel_path) DO UPDATE SET deleted_at = excluded.deleted_at",
    )
    .bind(user_id)
    .bind(rel_path)
    .bind(now_ms())
    .execute(&state.db)
    .await;
}

async fn clear_tombstone(state: &AppState, user_id: &str, rel_path: &str) {
    let _ = sqlx::query("DELETE FROM deleted_files WHERE user_id = ? AND rel_path = ?")
        .bind(user_id)
        .bind(rel_path)
        .execute(&state.db)
        .await;
}

/// Every file (not directory) under `dir`, as rel keys prefixed with
/// `rel_prefix` — used to tombstone the contents of a deleted/moved folder.
fn collect_files_under(dir: &Path, rel_prefix: &str, out: &mut Vec<String>) {
    let Ok(read) = fs::read_dir(dir) else { return };
    for entry in read.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        let rel = if rel_prefix.is_empty() {
            name
        } else {
            format!("{}/{}", rel_prefix, name)
        };
        if entry.path().is_dir() {
            collect_files_under(&entry.path(), &rel, out);
        } else {
            out.push(rel);
        }
    }
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

/// Cheap change-detection for one file: lets a client poll "did anyone else
/// write this?" without downloading the content or walking the whole tree.
#[derive(Serialize)]
pub struct FileStat {
    modified: u64,
    hash: String,
}

pub fn stat_file(path: &Path) -> Result<FileStat, AppError> {
    let metadata = fs::metadata(path)?;
    if !metadata.is_file() {
        return Err(AppError::NotFound);
    }
    Ok(FileStat {
        modified: modified_ms(&metadata),
        hash: hash_file(path),
    })
}

pub async fn stat_work_file(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<PathQuery>,
) -> Result<Json<FileStat>, AppError> {
    let root = user_root(&state, &session, &headers).await?;
    let rel = safe_rel_path(&q.path)?;
    Ok(Json(stat_file(&root.join(rel))?))
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
    let (user_id, root) = user_ctx(&state, &session, &headers).await?;
    let rel = safe_rel_path(&q.path)?;
    let path = root.join(&rel);

    check_quota(&state, &root, &path, content.len() as u64)?;

    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::write(path, content)?;

    // Writing a path makes it a live file again; a stale tombstone would
    // tell sync clients to delete what the user just (re)created.
    clear_tombstone(&state, &user_id, &rel_key(&rel)).await;
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
    let (user_id, root) = user_ctx(&state, &session, &headers).await?;
    let rel = safe_rel_path(&q.path)?;
    let path = root.join(&rel);
    let key = rel_key(&rel);

    if q.is_dir {
        let mut inside = Vec::new();
        collect_files_under(&path, &key, &mut inside);
        fs::remove_dir_all(path)?;
        for file_rel in inside {
            record_tombstone(&state, &user_id, &file_rel).await;
        }
    } else {
        fs::remove_file(path)?;
        record_tombstone(&state, &user_id, &key).await;
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
    let (user_id, root) = user_ctx(&state, &session, &headers).await?;
    let from_rel = safe_rel_path(&body.from)?;
    let to_rel = safe_rel_path(&body.to)?;
    let from = root.join(&from_rel);
    let to = root.join(&to_rel);
    let from_key = rel_key(&from_rel);
    let to_key = rel_key(&to_rel);

    // A move is a delete at `from` plus a create at `to`, tombstone-wise:
    // stale copies of the old path must not resurface, and any old tombstone
    // on the destination must not kill the file that now lives there.
    let mut moved_files = Vec::new();
    if from.is_dir() {
        collect_files_under(&from, &from_key, &mut moved_files);
    } else {
        moved_files.push(from_key.clone());
    }

    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::rename(from, to)?;

    for file_rel in moved_files {
        record_tombstone(&state, &user_id, &file_rel).await;
        let dest = format!("{}{}", to_key, &file_rel[from_key.len()..]);
        clear_tombstone(&state, &user_id, &dest).await;
    }
    Ok(StatusCode::OK)
}

#[derive(Serialize)]
pub struct DeletedEntry {
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "deletedAt")]
    deleted_at: i64,
}

/// Tombstone listing for sync clients: files this workspace deleted, with
/// when. Lets a client holding an unknown local copy distinguish "deleted
/// elsewhere, drop it" from "created locally, push it".
pub async fn list_deleted(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
) -> Result<Json<Vec<DeletedEntry>>, AppError> {
    let (user_id, _) = user_ctx(&state, &session, &headers).await?;
    let rows = sqlx::query_as::<_, (String, i64)>(
        "SELECT rel_path, deleted_at FROM deleted_files WHERE user_id = ?",
    )
    .bind(&user_id)
    .fetch_all(&state.db)
    .await?;
    Ok(Json(
        rows.into_iter()
            .map(|(rel_path, deleted_at)| DeletedEntry { rel_path, deleted_at })
            .collect(),
    ))
}
