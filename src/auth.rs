use argon2::password_hash::{PasswordHash, PasswordHasher, PasswordVerifier, SaltString};
use argon2::Argon2;
use axum::extract::State;
use axum::Json;
use rand_core::OsRng;
use serde::{Deserialize, Serialize};
use tower_sessions::Session;
use uuid::Uuid;

use crate::error::AppError;
use crate::state::AppState;

const SESSION_USER_ID_KEY: &str = "user_id";

#[derive(Deserialize)]
pub struct Credentials {
    pub email: String,
    pub password: String,
}

#[derive(Serialize)]
pub struct UserView {
    pub id: String,
    pub email: String,
}

/// Reads the authenticated user's id out of the session, or fails with
/// Unauthorized. Every workspace handler must go through this rather than
/// trusting any client-supplied identifier, since the workspace root on disk
/// is derived directly from this id.
pub async fn current_user_id(session: &Session) -> Result<String, AppError> {
    session
        .get::<String>(SESSION_USER_ID_KEY)
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?
        .ok_or(AppError::Unauthorized)
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

pub async fn register(
    State(state): State<AppState>,
    session: Session,
    Json(creds): Json<Credentials>,
) -> Result<Json<UserView>, AppError> {
    let email = creds.email.trim().to_lowercase();
    if email.is_empty() || !email.contains('@') {
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
    let now = time_now();

    sqlx::query("INSERT INTO users (id, email, password_hash, created_at) VALUES (?, ?, ?, ?)")
        .bind(&id)
        .bind(&email)
        .bind(&password_hash)
        .bind(now)
        .execute(&state.db)
        .await?;

    std::fs::create_dir_all(state.workspaces_dir.join(&id))?;

    session
        .insert(SESSION_USER_ID_KEY, id.clone())
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;

    Ok(Json(UserView { id, email }))
}

pub async fn login(
    State(state): State<AppState>,
    session: Session,
    Json(creds): Json<Credentials>,
) -> Result<Json<UserView>, AppError> {
    let email = creds.email.trim().to_lowercase();

    let row = sqlx::query_as::<_, (String, String, String)>(
        "SELECT id, email, password_hash FROM users WHERE email = ?",
    )
    .bind(&email)
    .fetch_optional(&state.db)
    .await?;

    let (id, email, password_hash) = row.ok_or(AppError::Unauthorized)?;

    if !verify_password(&creds.password, &password_hash)? {
        return Err(AppError::Unauthorized);
    }

    session
        .insert(SESSION_USER_ID_KEY, id.clone())
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;

    Ok(Json(UserView { id, email }))
}

pub async fn logout(session: Session) -> Result<StatusOk, AppError> {
    session
        .flush()
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;
    Ok(StatusOk)
}

pub async fn me(State(state): State<AppState>, session: Session) -> Result<Json<UserView>, AppError> {
    let id = current_user_id(&session).await?;

    let row = sqlx::query_as::<_, (String, String)>("SELECT id, email FROM users WHERE id = ?")
        .bind(&id)
        .fetch_optional(&state.db)
        .await?;

    let (id, email) = row.ok_or(AppError::Unauthorized)?;
    Ok(Json(UserView { id, email }))
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
