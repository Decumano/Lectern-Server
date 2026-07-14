// Sharing and comments. An owner can share a file or folder from their
// workspace with another registered account at one of three permission
// levels (view < comment < edit); grantees reach the shared content through
// the /api/shared/* endpoints, which resolve a share id (never a raw owner
// path) and enforce the granted level server-side. Comments are stored
// against the file's owner + path so everyone with access sees one thread,
// and are only allowed on Documents (.mdp).

use axum::extract::{ConnectInfo, Path as AxumPath, Query, State};
use axum::{http::HeaderMap, http::StatusCode, Json};
use serde::{Deserialize, Serialize};
use std::fs;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::Duration;
use tower_sessions::Session;
use uuid::Uuid;

use crate::auth::current_user_id_with_headers;
use crate::error::AppError;
use crate::state::AppState;
use crate::workspace::{safe_rel_path, walk_work_dir, FsEntry, WORK_FILE_EXTENSIONS};

const MAX_COMMENT_LEN: usize = 4000;

// Anonymous link visitors can post comments; cap the rate per IP so a leaked
// comment-link can't be used to flood a document.
const COMMENT_LIMIT: u32 = 20;
const COMMENT_WINDOW: Duration = Duration::from_secs(10 * 60);

/// Auth that doesn't fail: link-based endpoints work without an account, but
/// still attribute actions to the caller when they happen to be logged in.
async fn optional_user_id(state: &AppState, session: &Session, headers: &HeaderMap) -> Option<String> {
    current_user_id_with_headers(state, session, headers).await.ok()
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Perm {
    View,
    Comment,
    Edit,
}

impl Perm {
    fn parse(s: &str) -> Option<Perm> {
        match s {
            "view" => Some(Perm::View),
            "comment" => Some(Perm::Comment),
            "edit" => Some(Perm::Edit),
            _ => None,
        }
    }
    fn as_str(self) -> &'static str {
        match self {
            Perm::View => "view",
            Perm::Comment => "comment",
            Perm::Edit => "edit",
        }
    }
}

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn is_work_file(rel_path: &str) -> bool {
    std::path::Path::new(rel_path)
        .extension()
        .and_then(|e| e.to_str())
        .map(|e| e.to_lowercase())
        .map(|e| WORK_FILE_EXTENSIONS.contains(&e.as_str()))
        .unwrap_or(false)
}

fn is_document(rel_path: &str) -> bool {
    rel_path.to_lowercase().ends_with(".mdp")
}

// ── Share rows ──

#[derive(sqlx::FromRow)]
struct ShareRow {
    owner_id: String,
    grantee_id: String,
    rel_path: String,
    is_dir: bool,
    permission: String,
}

async fn load_share(state: &AppState, share_id: &str) -> Result<ShareRow, AppError> {
    sqlx::query_as::<_, ShareRow>(
        "SELECT owner_id, grantee_id, rel_path, is_dir, permission FROM shares WHERE id = ?",
    )
    .bind(share_id)
    .fetch_optional(&state.db)
    .await?
    .ok_or(AppError::NotFound)
}

/// Resolves a grantee's access through a share: checks the share belongs to
/// this user, joins the optional sub path (folder shares only) safely under
/// the shared root, and returns the owner-workspace-relative path plus the
/// granted permission.
async fn resolve_share_access(
    state: &AppState,
    user_id: &str,
    share_id: &str,
    sub_path: &str,
) -> Result<(ShareRow, String, Perm), AppError> {
    let share = load_share(state, share_id).await?;
    if share.grantee_id != user_id {
        return Err(AppError::NotFound); // don't reveal that the share exists
    }
    let perm = Perm::parse(&share.permission)
        .ok_or_else(|| AppError::Internal("bad permission in db".to_string()))?;

    let effective = if sub_path.is_empty() {
        share.rel_path.clone()
    } else {
        if !share.is_dir {
            return Err(AppError::BadRequest("not a folder share".to_string()));
        }
        let sub = safe_rel_path(sub_path)?;
        let mut p = PathBuf::from(&share.rel_path);
        p.push(sub);
        p.to_string_lossy().replace('\\', "/")
    };
    Ok((share, effective, perm))
}

fn owner_full_path(state: &AppState, owner_id: &str, rel: &str) -> Result<PathBuf, AppError> {
    // rel comes from the shares table (owner-created) plus a safe_rel_path'd
    // sub path, but re-validate the whole thing anyway before touching disk.
    let rel = safe_rel_path(rel)?;
    Ok(state.workspaces_dir.join(owner_id).join(rel))
}

// ── Link shares ("anyone with the link") ──

#[derive(sqlx::FromRow)]
struct LinkRow {
    owner_id: String,
    rel_path: String,
    is_dir: bool,
    permission: String,
}

async fn load_link(state: &AppState, token: &str) -> Result<LinkRow, AppError> {
    sqlx::query_as::<_, LinkRow>(
        "SELECT owner_id, rel_path, is_dir, permission FROM share_links WHERE id = ?",
    )
    .bind(token)
    .fetch_optional(&state.db)
    .await?
    .ok_or(AppError::NotFound)
}

/// Same shape as `resolve_share_access`, but the token itself is the
/// authorization — no session or account involved.
async fn resolve_link_access(
    state: &AppState,
    token: &str,
    sub_path: &str,
) -> Result<(LinkRow, String, Perm), AppError> {
    let link = load_link(state, token).await?;
    let perm = Perm::parse(&link.permission)
        .ok_or_else(|| AppError::Internal("bad permission in db".to_string()))?;

    let effective = if sub_path.is_empty() {
        link.rel_path.clone()
    } else {
        if !link.is_dir {
            return Err(AppError::BadRequest("not a folder link".to_string()));
        }
        let sub = safe_rel_path(sub_path)?;
        let mut p = PathBuf::from(&link.rel_path);
        p.push(sub);
        p.to_string_lossy().replace('\\', "/")
    };
    Ok((link, effective, perm))
}

// ── Owner endpoints: create / list / revoke shares ──

#[derive(Deserialize)]
pub struct CreateShareBody {
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    email: String,
    permission: String,
}

#[derive(Serialize)]
pub struct ShareView {
    id: String,
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    email: String,
    permission: String,
}

pub async fn create_share(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Json(body): Json<CreateShareBody>,
) -> Result<Json<ShareView>, AppError> {
    let owner_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let perm = Perm::parse(&body.permission)
        .ok_or_else(|| AppError::BadRequest("permission must be view, comment or edit".into()))?;

    let rel = safe_rel_path(&body.rel_path)?;
    let rel_str = rel.to_string_lossy().replace('\\', "/");
    let full = state.workspaces_dir.join(&owner_id).join(&rel);
    if body.is_dir && !full.is_dir() {
        return Err(AppError::BadRequest("no such folder".to_string()));
    }
    if !body.is_dir && !full.is_file() {
        return Err(AppError::BadRequest("no such file".to_string()));
    }

    let email = body.email.trim().to_lowercase();
    let grantee = sqlx::query_as::<_, (String, String)>("SELECT id, email FROM users WHERE email = ?")
        .bind(&email)
        .fetch_optional(&state.db)
        .await?
        .ok_or_else(|| AppError::BadRequest("no account with that email".to_string()))?;
    if grantee.0 == owner_id {
        return Err(AppError::BadRequest("that's you — no need to share".to_string()));
    }

    let id = Uuid::new_v4().to_string();
    // Upsert: re-sharing the same entry with the same person updates the level.
    sqlx::query(
        "INSERT INTO shares (id, owner_id, grantee_id, rel_path, is_dir, permission, created_at)
         VALUES (?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (owner_id, grantee_id, rel_path)
         DO UPDATE SET permission = excluded.permission, is_dir = excluded.is_dir",
    )
    .bind(&id)
    .bind(&owner_id)
    .bind(&grantee.0)
    .bind(&rel_str)
    .bind(body.is_dir)
    .bind(perm.as_str())
    .bind(now_ms())
    .execute(&state.db)
    .await?;

    // The upsert may have kept the original row id; read it back.
    let actual_id = sqlx::query_scalar::<_, String>(
        "SELECT id FROM shares WHERE owner_id = ? AND grantee_id = ? AND rel_path = ?",
    )
    .bind(&owner_id)
    .bind(&grantee.0)
    .bind(&rel_str)
    .fetch_one(&state.db)
    .await?;

    Ok(Json(ShareView {
        id: actual_id,
        rel_path: rel_str,
        is_dir: body.is_dir,
        email: grantee.1,
        permission: perm.as_str().to_string(),
    }))
}

#[derive(Deserialize)]
pub struct SharesForQuery {
    path: String,
}

pub async fn list_shares_for_entry(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<SharesForQuery>,
) -> Result<Json<Vec<ShareView>>, AppError> {
    let owner_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let rel_str = safe_rel_path(&q.path)?.to_string_lossy().replace('\\', "/");

    let rows = sqlx::query_as::<_, (String, String, bool, String, String)>(
        "SELECT s.id, s.rel_path, s.is_dir, s.permission, u.email
         FROM shares s JOIN users u ON u.id = s.grantee_id
         WHERE s.owner_id = ? AND s.rel_path = ?
         ORDER BY u.email",
    )
    .bind(&owner_id)
    .bind(&rel_str)
    .fetch_all(&state.db)
    .await?;

    Ok(Json(
        rows.into_iter()
            .map(|(id, rel_path, is_dir, permission, email)| ShareView {
                id,
                rel_path,
                is_dir,
                permission,
                email,
            })
            .collect(),
    ))
}

pub async fn revoke_share(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    AxumPath(share_id): AxumPath<String>,
) -> Result<StatusCode, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    // Either side can end a share: the owner revokes it, the grantee leaves.
    let res = sqlx::query("DELETE FROM shares WHERE id = ? AND (owner_id = ? OR grantee_id = ?)")
        .bind(&share_id)
        .bind(&user_id)
        .bind(&user_id)
        .execute(&state.db)
        .await?;
    if res.rows_affected() == 0 {
        return Err(AppError::NotFound);
    }
    Ok(StatusCode::OK)
}

// ── Link endpoints: owner management ──

#[derive(Deserialize)]
pub struct CreateLinkBody {
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    permission: String,
}

#[derive(Serialize)]
pub struct LinkView {
    /// Doubles as the URL token.
    id: String,
    #[serde(rename = "relPath")]
    rel_path: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    permission: String,
}

pub async fn create_link(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Json(body): Json<CreateLinkBody>,
) -> Result<Json<LinkView>, AppError> {
    let owner_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let perm = Perm::parse(&body.permission)
        .ok_or_else(|| AppError::BadRequest("permission must be view, comment or edit".into()))?;

    let rel = safe_rel_path(&body.rel_path)?;
    let rel_str = rel.to_string_lossy().replace('\\', "/");
    let full = state.workspaces_dir.join(&owner_id).join(&rel);
    if body.is_dir && !full.is_dir() {
        return Err(AppError::BadRequest("no such folder".to_string()));
    }
    if !body.is_dir && !full.is_file() {
        return Err(AppError::BadRequest("no such file".to_string()));
    }

    let id = Uuid::new_v4().to_string();
    sqlx::query(
        "INSERT INTO share_links (id, owner_id, rel_path, is_dir, permission, created_at)
         VALUES (?, ?, ?, ?, ?, ?)",
    )
    .bind(&id)
    .bind(&owner_id)
    .bind(&rel_str)
    .bind(body.is_dir)
    .bind(perm.as_str())
    .bind(now_ms())
    .execute(&state.db)
    .await?;

    Ok(Json(LinkView {
        id,
        rel_path: rel_str,
        is_dir: body.is_dir,
        permission: perm.as_str().to_string(),
    }))
}

pub async fn list_links_for_entry(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<SharesForQuery>,
) -> Result<Json<Vec<LinkView>>, AppError> {
    let owner_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let rel_str = safe_rel_path(&q.path)?.to_string_lossy().replace('\\', "/");

    let rows = sqlx::query_as::<_, (String, String, bool, String)>(
        "SELECT id, rel_path, is_dir, permission FROM share_links
         WHERE owner_id = ? AND rel_path = ? ORDER BY created_at",
    )
    .bind(&owner_id)
    .bind(&rel_str)
    .fetch_all(&state.db)
    .await?;

    Ok(Json(
        rows.into_iter()
            .map(|(id, rel_path, is_dir, permission)| LinkView { id, rel_path, is_dir, permission })
            .collect(),
    ))
}

pub async fn revoke_link(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    AxumPath(link_id): AxumPath<String>,
) -> Result<StatusCode, AppError> {
    let owner_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let res = sqlx::query("DELETE FROM share_links WHERE id = ? AND owner_id = ?")
        .bind(&link_id)
        .bind(&owner_id)
        .execute(&state.db)
        .await?;
    if res.rows_affected() == 0 {
        return Err(AppError::NotFound);
    }
    Ok(StatusCode::OK)
}

// ── Link endpoints: anonymous access (the token is the authorization) ──

#[derive(Serialize)]
pub struct LinkMetaView {
    name: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    permission: String,
    #[serde(rename = "ownerEmail")]
    owner_email: String,
    /// False when the owner has since deleted/moved the linked entry.
    exists: bool,
}

pub async fn link_meta(
    State(state): State<AppState>,
    AxumPath(token): AxumPath<String>,
) -> Result<Json<LinkMetaView>, AppError> {
    let link = load_link(&state, &token).await?;
    let owner_email = sqlx::query_scalar::<_, String>("SELECT email FROM users WHERE id = ?")
        .bind(&link.owner_id)
        .fetch_one(&state.db)
        .await?;
    let full = owner_full_path(&state, &link.owner_id, &link.rel_path)?;
    let exists = if link.is_dir { full.is_dir() } else { full.is_file() };
    let name = link.rel_path.rsplit('/').next().unwrap_or(&link.rel_path).to_string();
    Ok(Json(LinkMetaView {
        name,
        is_dir: link.is_dir,
        permission: link.permission,
        owner_email,
        exists,
    }))
}

#[derive(Deserialize)]
pub struct LinkSubQuery {
    #[serde(default)]
    path: String,
}

pub async fn link_list_folder(
    State(state): State<AppState>,
    AxumPath(token): AxumPath<String>,
    Query(q): Query<LinkSubQuery>,
) -> Result<Json<Vec<FsEntry>>, AppError> {
    let (link, effective, _perm) = resolve_link_access(&state, &token, &q.path).await?;
    if !link.is_dir {
        return Err(AppError::BadRequest("not a folder link".to_string()));
    }
    let full = owner_full_path(&state, &link.owner_id, &effective)?;
    if !full.is_dir() {
        return Err(AppError::NotFound);
    }
    Ok(Json(walk_work_dir(&full, "")?))
}

pub async fn link_read_file(
    State(state): State<AppState>,
    AxumPath(token): AxumPath<String>,
    Query(q): Query<LinkSubQuery>,
) -> Result<String, AppError> {
    let (link, effective, _perm) = resolve_link_access(&state, &token, &q.path).await?;
    if !is_work_file(&effective) {
        return Err(AppError::BadRequest("not a work file".to_string()));
    }
    let full = owner_full_path(&state, &link.owner_id, &effective)?;
    fs::read_to_string(full).map_err(|e| e.into())
}

pub async fn link_write_file(
    State(state): State<AppState>,
    AxumPath(token): AxumPath<String>,
    Query(q): Query<LinkSubQuery>,
    content: String,
) -> Result<StatusCode, AppError> {
    let (link, effective, perm) = resolve_link_access(&state, &token, &q.path).await?;
    if perm < Perm::Edit {
        return Err(AppError::Forbidden);
    }
    if !is_work_file(&effective) {
        return Err(AppError::BadRequest("not a work file".to_string()));
    }
    let full = owner_full_path(&state, &link.owner_id, &effective)?;
    // Same rule as account shares: edit means edit, not create.
    if !full.is_file() {
        return Err(AppError::NotFound);
    }
    fs::write(full, content)?;
    Ok(StatusCode::OK)
}

// ── Grantee endpoints: browse and use what's shared with me ──

#[derive(Serialize)]
pub struct SharedWithMeView {
    #[serde(rename = "shareId")]
    share_id: String,
    #[serde(rename = "ownerEmail")]
    owner_email: String,
    name: String,
    #[serde(rename = "isDir")]
    is_dir: bool,
    permission: String,
    /// False when the owner has since deleted/moved the shared entry.
    exists: bool,
}

pub async fn list_shared_with_me(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
) -> Result<Json<Vec<SharedWithMeView>>, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;

    let rows = sqlx::query_as::<_, (String, String, String, bool, String, String)>(
        "SELECT s.id, s.owner_id, s.rel_path, s.is_dir, s.permission, u.email
         FROM shares s JOIN users u ON u.id = s.owner_id
         WHERE s.grantee_id = ?
         ORDER BY u.email, s.rel_path",
    )
    .bind(&user_id)
    .fetch_all(&state.db)
    .await?;

    let mut out = Vec::new();
    for (id, owner_id, rel_path, is_dir, permission, owner_email) in rows {
        let full = owner_full_path(&state, &owner_id, &rel_path)?;
        let exists = if is_dir { full.is_dir() } else { full.is_file() };
        let name = rel_path.rsplit('/').next().unwrap_or(&rel_path).to_string();
        out.push(SharedWithMeView {
            share_id: id,
            owner_email,
            name,
            is_dir,
            permission,
            exists,
        });
    }
    Ok(Json(out))
}

#[derive(Deserialize)]
pub struct SharedQuery {
    share: String,
    #[serde(default)]
    path: String,
}

pub async fn list_shared_folder(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<SharedQuery>,
) -> Result<Json<Vec<FsEntry>>, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let (share, effective, _perm) = resolve_share_access(&state, &user_id, &q.share, &q.path).await?;
    if !share.is_dir {
        return Err(AppError::BadRequest("not a folder share".to_string()));
    }
    let full = owner_full_path(&state, &share.owner_id, &effective)?;
    if !full.is_dir() {
        return Err(AppError::NotFound);
    }
    Ok(Json(walk_work_dir(&full, "")?))
}

pub async fn read_shared_file(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<SharedQuery>,
) -> Result<String, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let (share, effective, _perm) = resolve_share_access(&state, &user_id, &q.share, &q.path).await?;
    if !is_work_file(&effective) {
        return Err(AppError::BadRequest("not a work file".to_string()));
    }
    let full = owner_full_path(&state, &share.owner_id, &effective)?;
    fs::read_to_string(full).map_err(|e| e.into())
}

pub async fn write_shared_file(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<SharedQuery>,
    content: String,
) -> Result<StatusCode, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    let (share, effective, perm) = resolve_share_access(&state, &user_id, &q.share, &q.path).await?;
    if perm < Perm::Edit {
        return Err(AppError::Forbidden);
    }
    if !is_work_file(&effective) {
        return Err(AppError::BadRequest("not a work file".to_string()));
    }
    let full = owner_full_path(&state, &share.owner_id, &effective)?;
    // Grantees can edit existing files but not create new ones in the owner's
    // workspace; keeps an edit share from being used to fill someone's disk.
    if !full.is_file() {
        return Err(AppError::NotFound);
    }
    fs::write(full, content)?;
    Ok(StatusCode::OK)
}

// ── Comments (Documents only) ──

#[derive(Deserialize)]
pub struct CommentsQuery {
    /// Owner context: path of my own file. Mutually exclusive with the others.
    #[serde(default)]
    path: String,
    /// Grantee context: share id (+ optional sub path for folder shares).
    #[serde(default)]
    share: String,
    /// Link context: link token (+ optional sub path). Works without a login.
    #[serde(default)]
    link: String,
    #[serde(default, rename = "subPath")]
    sub_path: String,
}

/// Works out which file a comments request is about and whether the caller
/// may see / post at the required level. `user_id` is None for anonymous
/// link visitors, which is only acceptable in the link context.
async fn resolve_comment_target(
    state: &AppState,
    user_id: Option<&str>,
    q: &CommentsQuery,
    need: Perm,
) -> Result<(String, String), AppError> {
    if !q.link.is_empty() {
        let (link, effective, perm) = resolve_link_access(state, &q.link, &q.sub_path).await?;
        if perm < need {
            return Err(AppError::Forbidden);
        }
        return Ok((link.owner_id, effective));
    }

    let user_id = user_id.ok_or(AppError::Unauthorized)?;
    if !q.share.is_empty() {
        let (share, effective, perm) =
            resolve_share_access(state, user_id, &q.share, &q.sub_path).await?;
        if perm < need {
            return Err(AppError::Forbidden);
        }
        Ok((share.owner_id, effective))
    } else {
        let rel = safe_rel_path(&q.path)?.to_string_lossy().replace('\\', "/");
        Ok((user_id.to_string(), rel))
    }
}

#[derive(Serialize)]
pub struct CommentView {
    id: String,
    /// None for comments left by anonymous link visitors.
    #[serde(rename = "authorEmail")]
    author_email: Option<String>,
    body: String,
    #[serde(rename = "createdAt")]
    created_at: i64,
    /// True when the caller wrote this comment (so the UI can offer delete).
    mine: bool,
}

pub async fn list_comments(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    Query(q): Query<CommentsQuery>,
) -> Result<Json<Vec<CommentView>>, AppError> {
    let user_id = optional_user_id(&state, &session, &headers).await;
    let (owner_id, rel_path) =
        resolve_comment_target(&state, user_id.as_deref(), &q, Perm::View).await?;
    if !is_document(&rel_path) {
        return Err(AppError::BadRequest("comments are only available on Documents".into()));
    }

    let rows = sqlx::query_as::<_, (String, Option<String>, String, i64, Option<String>)>(
        "SELECT c.id, u.email, c.body, c.created_at, c.author_id
         FROM comments c LEFT JOIN users u ON u.id = c.author_id
         WHERE c.owner_id = ? AND c.rel_path = ?
         ORDER BY c.created_at",
    )
    .bind(&owner_id)
    .bind(&rel_path)
    .fetch_all(&state.db)
    .await?;

    Ok(Json(
        rows.into_iter()
            .map(|(id, author_email, body, created_at, author_id)| CommentView {
                id,
                author_email,
                body,
                created_at,
                mine: author_id.is_some() && author_id == user_id,
            })
            .collect(),
    ))
}

#[derive(Deserialize)]
pub struct CreateCommentBody {
    #[serde(flatten)]
    target: CommentsQuery,
    body: String,
}

pub async fn create_comment(
    State(state): State<AppState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    session: Session,
    headers: HeaderMap,
    Json(body): Json<CreateCommentBody>,
) -> Result<Json<CommentView>, AppError> {
    let user_id = optional_user_id(&state, &session, &headers).await;

    // Anonymous posting is only reachable through comment/edit links; keep a
    // per-IP lid on it since the author can't be held accountable otherwise.
    if user_id.is_none() {
        let ip = addr.ip();
        if state
            .auth_limiter
            .is_limited(ip, "anon-comment", COMMENT_LIMIT, COMMENT_WINDOW)
        {
            return Err(AppError::TooManyRequests);
        }
        state.auth_limiter.record(ip, "anon-comment", COMMENT_WINDOW);
    }

    let (owner_id, rel_path) =
        resolve_comment_target(&state, user_id.as_deref(), &body.target, Perm::Comment).await?;
    if !is_document(&rel_path) {
        return Err(AppError::BadRequest("comments are only available on Documents".into()));
    }

    let text = body.body.trim();
    if text.is_empty() {
        return Err(AppError::BadRequest("empty comment".to_string()));
    }
    if text.len() > MAX_COMMENT_LEN {
        return Err(AppError::BadRequest("comment too long".to_string()));
    }
    // The file must actually exist to be commented on.
    let full = owner_full_path(&state, &owner_id, &rel_path)?;
    if !full.is_file() {
        return Err(AppError::NotFound);
    }

    let id = Uuid::new_v4().to_string();
    let created_at = now_ms();
    sqlx::query(
        "INSERT INTO comments (id, owner_id, rel_path, author_id, body, created_at)
         VALUES (?, ?, ?, ?, ?, ?)",
    )
    .bind(&id)
    .bind(&owner_id)
    .bind(&rel_path)
    .bind(&user_id)
    .bind(text)
    .bind(created_at)
    .execute(&state.db)
    .await?;

    let email = match &user_id {
        Some(uid) => sqlx::query_scalar::<_, String>("SELECT email FROM users WHERE id = ?")
            .bind(uid)
            .fetch_optional(&state.db)
            .await?,
        None => None,
    };

    Ok(Json(CommentView {
        id,
        author_email: email,
        body: text.to_string(),
        created_at,
        mine: user_id.is_some(),
    }))
}

pub async fn delete_comment(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
    AxumPath(comment_id): AxumPath<String>,
) -> Result<StatusCode, AppError> {
    let user_id = current_user_id_with_headers(&state, &session, &headers).await?;
    // The comment's author or the file's owner may delete it.
    let res = sqlx::query("DELETE FROM comments WHERE id = ? AND (author_id = ? OR owner_id = ?)")
        .bind(&comment_id)
        .bind(&user_id)
        .bind(&user_id)
        .execute(&state.db)
        .await?;
    if res.rows_affected() == 0 {
        return Err(AppError::NotFound);
    }
    Ok(StatusCode::OK)
}
