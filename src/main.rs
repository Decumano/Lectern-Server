mod auth;
mod db;
mod downloads;
mod error;
mod export;
mod share;
mod state;
mod workspace;

use axum::extract::{DefaultBodyLimit, Request};
use axum::http::{header, HeaderValue};
use axum::middleware::{self, Next};
use axum::response::Response;
use axum::routing::{delete, get, post};
use axum::Router;
use state::{AppState, RateLimiter};
use std::net::SocketAddr;
use std::path::PathBuf;
use tower_http::services::{ServeDir, ServeFile};
use tower_http::trace::TraceLayer;
use tower_sessions::cookie::time::Duration;
use tower_sessions::cookie::Key;
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

    // Session cookies are only marked Secure when COOKIE_SECURE=true; set it
    // once the server sits behind HTTPS (Secure cookies never arrive over
    // plain http://, which would break local development).
    let cookie_secure = std::env::var("COOKIE_SECURE")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);

    let session_layer = SessionManagerLayer::new(session_store)
        .with_secure(cookie_secure)
        .with_expiry(Expiry::OnInactivity(Duration::days(30)));

    let state = AppState {
        db,
        workspaces_dir,
        auth_limiter: RateLimiter::default(),
        releases: std::sync::Arc::new(downloads::Releases::from_env()),
    };

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
        .route("/workspace/move", post(workspace::move_work_entry))
        .route(
            "/shares",
            post(share::create_share).get(share::list_shares_for_entry),
        )
        .route("/shares/:id", delete(share::revoke_share))
        .route("/shared", get(share::list_shared_with_me))
        .route("/shared/list", get(share::list_shared_folder))
        .route(
            "/shared/file",
            get(share::read_shared_file).put(share::write_shared_file),
        )
        .route(
            "/links",
            post(share::create_link).get(share::list_links_for_entry),
        )
        .route("/links/:id", delete(share::revoke_link))
        .route("/link/:token", get(share::link_meta))
        .route("/link/:token/list", get(share::link_list_folder))
        .route(
            "/link/:token/file",
            get(share::link_read_file).put(share::link_write_file),
        )
        .route(
            "/comments",
            get(share::list_comments).post(share::create_comment),
        )
        .route("/comments/:id", delete(share::delete_comment))
        .route("/downloads/latest", get(downloads::latest_release))
        .route(
            "/downloads/asset/:id/:name",
            get(downloads::download_asset),
        )
        .route(
            "/export/pdf",
            post(export::export_pdf).layer(DefaultBodyLimit::max(50 * 1024 * 1024)),
        );

    let static_files = ServeDir::new("web").fallback(ServeFile::new("web/index.html"));

    let app = Router::new()
        .nest("/api", api)
        .fallback_service(static_files);

    // SESSION_SECRET signs the session cookie so a tampered or forged cookie
    // is rejected outright. Optional for local development (unsigned cookies
    // plus a warning), strongly recommended for any real deployment.
    let app = match std::env::var("SESSION_SECRET") {
        Ok(secret) if !secret.is_empty() => {
            let key = Key::try_from(secret.as_bytes()).map_err(|_| {
                anyhow::anyhow!("SESSION_SECRET must be at least 64 bytes of random data")
            })?;
            app.layer(session_layer.with_signed(key))
        }
        _ => {
            tracing::warn!(
                "SESSION_SECRET is not set; session cookies are unsigned (fine for local dev)"
            );
            app.layer(session_layer)
        }
    };

    // No CORS layer on purpose: the browser frontend is served same-origin
    // from this process, and the desktop app calls the API from Rust (reqwest),
    // which isn't subject to CORS. Not exposing cross-origin access is the
    // safer default for a self-hosted instance.
    let app = app
        .layer(middleware::from_fn(static_cache_control))
        .layer(TraceLayer::new_for_http())
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(("0.0.0.0", port)).await?;
    tracing::info!("officesuite-web listening on http://0.0.0.0:{port}");
    axum::serve(
        listener,
        app.into_make_service_with_connect_info::<SocketAddr>(),
    )
    .await?;

    Ok(())
}

/// Cache policy for the static frontend. Browsers heuristically cache
/// responses that carry no Cache-Control at all (10% of file age), which
/// meant a deployed update could serve a stale index.html — and with it,
/// stale everything — for days. HTML entry points must always revalidate
/// (`no-cache` still allows 304s, so it stays cheap); other assets get an
/// hour, and index.html's `?v=N` query params handle hard busts on deploys.
async fn static_cache_control(req: Request, next: Next) -> Response {
    let path = req.uri().path().to_string();
    let mut res = next.run(req).await;
    if !path.starts_with("/api") {
        let is_html = path == "/" || path.ends_with(".html") || !path.contains('.');
        let value = if is_html {
            "no-cache"
        } else {
            "public, max-age=3600, must-revalidate"
        };
        res.headers_mut()
            .insert(header::CACHE_CONTROL, HeaderValue::from_static(value));
    }
    res
}
