use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde_json::json;

pub enum AppError {
    BadRequest(String),
    Unauthorized,
    Forbidden,
    NotFound,
    Conflict(String),
    TooManyRequests,
    Internal(String),
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        let (status, message) = match self {
            AppError::BadRequest(msg) => (StatusCode::BAD_REQUEST, msg),
            AppError::Unauthorized => (StatusCode::UNAUTHORIZED, "unauthorized".to_string()),
            AppError::Forbidden => (
                StatusCode::FORBIDDEN,
                "you don't have permission to do that".to_string(),
            ),
            AppError::NotFound => (StatusCode::NOT_FOUND, "not found".to_string()),
            AppError::Conflict(msg) => (StatusCode::CONFLICT, msg),
            AppError::TooManyRequests => (
                StatusCode::TOO_MANY_REQUESTS,
                "too many attempts, try again later".to_string(),
            ),
            AppError::Internal(msg) => (StatusCode::INTERNAL_SERVER_ERROR, msg),
        };

        (status, Json(json!({ "error": message }))).into_response()
    }
}

impl From<sqlx::Error> for AppError {
    fn from(err: sqlx::Error) -> Self {
        AppError::Internal(err.to_string())
    }
}

impl From<std::io::Error> for AppError {
    fn from(err: std::io::Error) -> Self {
        if err.kind() == std::io::ErrorKind::NotFound {
            AppError::NotFound
        } else {
            AppError::Internal(err.to_string())
        }
    }
}
