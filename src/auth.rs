use argon2::password_hash::{PasswordHash, PasswordHasher, PasswordVerifier, SaltString};
use argon2::Argon2;
use axum::extract::{ConnectInfo, State};
use axum::http::HeaderMap;
use axum::Json;
use rand_core::OsRng;
use serde::{Deserialize, Serialize};
use std::net::SocketAddr;
use std::sync::OnceLock;
use std::time::Duration;
use tower_sessions::Session;
use uuid::Uuid;

use crate::error::AppError;
use crate::state::AppState;

const SESSION_USER_ID_KEY: &str = "user_id";

// Per-IP abuse limits: 10 failed logins per 15 minutes blocks password
// brute force without ever counting successful logins (so a shared proxy IP
// doesn't lock out legitimate users); 20 registrations per hour blocks mass
// account creation.
const LOGIN_FAIL_LIMIT: u32 = 10;
const LOGIN_FAIL_WINDOW: Duration = Duration::from_secs(15 * 60);
const REGISTER_LIMIT: u32 = 20;
const REGISTER_WINDOW: Duration = Duration::from_secs(60 * 60);

#[derive(Deserialize)]
pub struct Credentials {
    pub email: String,
    pub password: String,
}

#[derive(Serialize)]
pub struct UserView {
    pub id: String,
    pub email: String,
    #[serde(rename = "apiToken")]
    pub api_token: String,
}

/// Reads the authenticated user's id, preferring an `Authorization: Bearer
/// <api_token>` header over the session cookie. The desktop app authenticates
/// this way because its webview origin can't rely on the session cookie (see
/// main.rs: sessions are SameSite=Strict, which cross-origin requests from a
/// Tauri webview won't carry). Browser clients keep using the cookie.
pub async fn current_user_id_with_headers(
    state: &AppState,
    session: &Session,
    headers: &HeaderMap,
) -> Result<String, AppError> {
    if let Some(token) = headers
        .get(axum::http::header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
    {
        let id = sqlx::query_scalar::<_, String>("SELECT id FROM users WHERE api_token = ?")
            .bind(token)
            .fetch_optional(&state.db)
            .await?
            .ok_or(AppError::Unauthorized)?;
        return Ok(id);
    }

    current_user_id(session).await
}

/// Reads the authenticated user's id out of the session, or fails with
/// Unauthorized. Kept separate from `current_user_id_with_headers` for
/// handlers (like auth::me) that only ever run against a cookie session.
pub async fn current_user_id(session: &Session) -> Result<String, AppError> {
    session
        .get::<String>(SESSION_USER_ID_KEY)
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?
        .ok_or(AppError::Unauthorized)
}

fn generate_api_token() -> String {
    Uuid::new_v4().to_string()
}

/// A deliberately conservative email check: exactly one `@`, non-empty local
/// and domain parts, a dot in the domain, a sane length, and no characters
/// that don't belong in an address. The frontend HTML-escapes emails wherever
/// it renders them, so this is defense-in-depth — it keeps hostile or
/// malformed values (e.g. ones carrying markup) out of the database entirely
/// rather than relying on every render site to escape.
fn is_valid_email(email: &str) -> bool {
    if email.len() < 3 || email.len() > 254 {
        return false;
    }
    // No whitespace or angle brackets/quotes that only ever show up in attacks.
    if email
        .chars()
        .any(|c| c.is_whitespace() || matches!(c, '<' | '>' | '"' | '\'' | '`' | '\\' | '\0'))
    {
        return false;
    }
    let mut parts = email.split('@');
    let (Some(local), Some(domain), None) = (parts.next(), parts.next(), parts.next()) else {
        return false; // zero or more than one '@'
    };
    !local.is_empty() && domain.contains('.') && !domain.starts_with('.') && !domain.ends_with('.')
}

fn hash_password(password: &str) -> Result<String, AppError> {
    let salt = SaltString::generate(&mut OsRng);
    Argon2::default()
        .hash_password(password.as_bytes(), &salt)
        .map(|h| h.to_string())
        .map_err(|e| AppError::Internal(e.to_string()))
}

fn verify_password(password: &str, hash: &str) -> Result<bool, AppError> {
    let parsed = PasswordHash::new(hash).map_err(|e| AppError::Internal(e.to_string()))?;
    Ok(Argon2::default()
        .verify_password(password.as_bytes(), &parsed)
        .is_ok())
}

/// Burns the same time as a real password check, so a login against an
/// unknown email doesn't return measurably faster than one against a wrong
/// password — which would let an attacker probe which emails have accounts.
fn dummy_verify(password: &str) {
    static DUMMY_HASH: OnceLock<String> = OnceLock::new();
    let hash =
        DUMMY_HASH.get_or_init(|| hash_password("dummy-timing-pad").unwrap_or_default());
    let _ = verify_password(password, hash);
}

pub async fn register(
    State(state): State<AppState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    session: Session,
    headers: HeaderMap,
    Json(creds): Json<Credentials>,
) -> Result<Json<UserView>, AppError> {
    let ip = state.client_ip(&addr, &headers);
    if state
        .auth_limiter
        .is_limited(ip, "register", REGISTER_LIMIT, REGISTER_WINDOW)
    {
        return Err(AppError::TooManyRequests);
    }
    state.auth_limiter.record(ip, "register", REGISTER_WINDOW);

    let email = creds.email.trim().to_lowercase();
    if !is_valid_email(&email) {
        return Err(AppError::BadRequest("invalid email".to_string()));
    }
    if creds.password.len() < 8 {
        return Err(AppError::BadRequest(
            "password must be at least 8 characters".to_string(),
        ));
    }

    let existing = sqlx::query_scalar::<_, i64>("SELECT COUNT(*) FROM users WHERE email = ?")
        .bind(&email)
        .fetch_one(&state.db)
        .await?;
    if existing > 0 {
        return Err(AppError::Conflict("email already registered".to_string()));
    }

    let id = Uuid::new_v4().to_string();
    let password_hash = hash_password(&creds.password)?;
    let api_token = generate_api_token();
    let now = time_now();

    sqlx::query(
        "INSERT INTO users (id, email, password_hash, created_at, api_token) VALUES (?, ?, ?, ?, ?)",
    )
    .bind(&id)
    .bind(&email)
    .bind(&password_hash)
    .bind(now)
    .bind(&api_token)
    .execute(&state.db)
    .await?;

    std::fs::create_dir_all(state.workspaces_dir.join(&id))?;

    session
        .insert(SESSION_USER_ID_KEY, id.clone())
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;

    Ok(Json(UserView { id, email, api_token }))
}

pub async fn login(
    State(state): State<AppState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
    session: Session,
    headers: HeaderMap,
    Json(creds): Json<Credentials>,
) -> Result<Json<UserView>, AppError> {
    let ip = state.client_ip(&addr, &headers);
    if state
        .auth_limiter
        .is_limited(ip, "login-fail", LOGIN_FAIL_LIMIT, LOGIN_FAIL_WINDOW)
    {
        return Err(AppError::TooManyRequests);
    }

    let email = creds.email.trim().to_lowercase();

    let row = sqlx::query_as::<_, (String, String, String, String)>(
        "SELECT id, email, password_hash, api_token FROM users WHERE email = ?",
    )
    .bind(&email)
    .fetch_optional(&state.db)
    .await?;

    let Some((id, email, password_hash, mut api_token)) = row else {
        dummy_verify(&creds.password);
        state.auth_limiter.record(ip, "login-fail", LOGIN_FAIL_WINDOW);
        return Err(AppError::Unauthorized);
    };

    if !verify_password(&creds.password, &password_hash)? {
        state.auth_limiter.record(ip, "login-fail", LOGIN_FAIL_WINDOW);
        return Err(AppError::Unauthorized);
    }

    // Users created before the api_token column existed have '' here; mint
    // one lazily so every account ends up with a token without a backfill script.
    if api_token.is_empty() {
        api_token = generate_api_token();
        sqlx::query("UPDATE users SET api_token = ? WHERE id = ?")
            .bind(&api_token)
            .bind(&id)
            .execute(&state.db)
            .await?;
    }

    session
        .insert(SESSION_USER_ID_KEY, id.clone())
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;

    Ok(Json(UserView { id, email, api_token }))
}

pub async fn logout(session: Session) -> Result<StatusOk, AppError> {
    session
        .flush()
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;
    Ok(StatusOk)
}

/// Identity without the API token. `/me` is reachable with nothing but the
/// session cookie, so returning the long-lived bearer token here would let
/// any script running on the page trade same-origin access for a permanent,
/// non-expiring credential. The desktop app receives its token from
/// login/register, which require the password, and doesn't call this.
#[derive(Serialize)]
pub struct MeView {
    pub id: String,
    pub email: String,
}

pub async fn me(
    State(state): State<AppState>,
    session: Session,
    headers: HeaderMap,
) -> Result<Json<MeView>, AppError> {
    let id = current_user_id_with_headers(&state, &session, &headers).await?;

    let row = sqlx::query_as::<_, (String, String)>("SELECT id, email FROM users WHERE id = ?")
        .bind(&id)
        .fetch_optional(&state.db)
        .await?;

    let (id, email) = row.ok_or(AppError::Unauthorized)?;
    Ok(Json(MeView { id, email }))
}

fn time_now() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

/// Bare 200 OK with no body, for endpoints that only need to signal success.
pub struct StatusOk;

impl axum::response::IntoResponse for StatusOk {
    fn into_response(self) -> axum::response::Response {
        axum::http::StatusCode::OK.into_response()
    }
}

#[cfg(test)]
mod tests {
    use super::is_valid_email;

    #[test]
    fn accepts_normal_emails() {
        assert!(is_valid_email("marc@example.com"));
        assert!(is_valid_email("a.b+tag@sub.domain.co.uk"));
    }

    #[test]
    fn rejects_malformed_and_hostile_emails() {
        assert!(!is_valid_email(""));
        assert!(!is_valid_email("no-at-sign"));
        assert!(!is_valid_email("two@@example.com"));
        assert!(!is_valid_email("a@b@c.com"));
        assert!(!is_valid_email("nodot@localhost"));
        assert!(!is_valid_email("@example.com"));
        assert!(!is_valid_email("x@.com"));
        assert!(!is_valid_email("x@com."));
        // markup / injection attempts must never enter the DB
        assert!(!is_valid_email("x\"<img src=x onerror=alert(1)>@e.com"));
        assert!(!is_valid_email("a b@example.com"));
    }
}
