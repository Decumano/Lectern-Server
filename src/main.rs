mod auth;
mod db;
mod error;
mod state;
mod workspace;

use axum::routing::{delete, get, post};
use axum::Router;
use state::AppState;
use std::path::PathBuf;
use tower_http::services::{ServeDir, ServeFile};
use tower_http::trace::TraceLayer;
use tower_sessions::cookie::time::Duration;
use tower_sessions::{Expiry, SessionManagerLayer};
use tower_sessions_sqlx_store::SqliteStore;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let _ = dotenvy::dotenv();
    tracing_subscriber::fmt::init();

    let database_url =
        std::env::var("DATABASE_URL").unwrap_or_else(|_| "sqlite://data/officesuite.db".into());
    let workspaces_dir =
        PathBuf::from(std::env::var("WORKSPACES_DIR").unwrap_or_else(|_| "data/workspaces".into()));
    let port: u16 = std::env::var("PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(8080);

    std::fs::create_dir_all(&workspaces_dir)?;

    let db = db::connect(&database_url).await?;

    let session_store = SqliteStore::new(db.clone());
    session_store.migrate().await?;

    let session_layer = SessionManagerLayer::new(session_store)
        .with_secure(false) // set true once served over HTTPS in production
        .with_expiry(Expiry::OnInactivity(Duration::days(30)));

    let state = AppState { db, workspaces_dir };

    let api = Router::new()
        .route("/auth/register", post(auth::register))
        .route("/auth/login", post(auth::login))
        .route("/auth/logout", post(auth::logout))
        .route("/auth/me", get(auth::me))
        .route("/workspace", get(workspace::list_work_folder))
        .route(
            "/workspace/file",
            get(workspace::read_work_file).put(workspace::write_work_file),
        )
        .route("/workspace/folder", post(workspace::create_work_folder))
        .route("/workspace/entry", delete(workspace::delete_work_entry))
        .route("/workspace/move", post(workspace::move_work_entry));

    let static_files = ServeDir::new("web").fallback(ServeFile::new("web/index.html"));

    let app = Router::new()
        .nest("/api", api)
        .fallback_service(static_files)
        .layer(session_layer)
        .layer(TraceLayer::new_for_http())
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(("0.0.0.0", port)).await?;
    tracing::info!("officesuite-web listening on http://0.0.0.0:{port}");
    axum::serve(listener, app).await?;

    Ok(())
}
