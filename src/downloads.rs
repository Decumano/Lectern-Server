// Server-side bridge to the GitHub Releases API for the Download page.
// The desktop-app repo can stay private: this server holds the GitHub token
// (GITHUB_RELEASES_TOKEN, never sent to browsers), lists the latest release,
// and streams the installer assets through to visitors. Only the one repo
// configured at startup is reachable, and assets are addressed by numeric id,
// so this is not an open proxy. With a public repo the token is unnecessary —
// the bridge then just saves visitors from GitHub's anonymous rate limits.

use axum::body::Body;
use axum::extract::{Path as AxumPath, State};
use axum::http::{header, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde::Serialize;
use serde_json::Value;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use crate::error::AppError;
use crate::state::AppState;

/// How long a fetched release listing is served from memory before asking
/// GitHub again. Keeps a busy download page to ~12 API calls per hour.
const CACHE_TTL: Duration = Duration::from_secs(300);

pub struct Releases {
    repo: String,
    token: Option<String>,
    client: reqwest::Client,
    cache: Mutex<Option<(Instant, LatestRelease)>>,
}

#[derive(Serialize, Clone)]
pub struct LatestRelease {
    #[serde(rename = "tagName")]
    tag_name: String,
    #[serde(rename = "publishedAt")]
    published_at: String,
    assets: Vec<AssetView>,
}

#[derive(Serialize, Clone)]
pub struct AssetView {
    id: u64,
    name: String,
    size: u64,
}

impl Releases {
    pub fn from_env() -> Releases {
        Releases {
            repo: std::env::var("DESKTOP_RELEASES_REPO")
                .unwrap_or_else(|_| "Decumano/OfficeSuite".into()),
            token: std::env::var("GITHUB_RELEASES_TOKEN").ok().filter(|t| !t.is_empty()),
            client: reqwest::Client::new(),
            cache: Mutex::new(None),
        }
    }

    fn github_get(&self, url: &str, accept: &'static str) -> reqwest::RequestBuilder {
        let mut req = self
            .client
            .get(url)
            // GitHub rejects requests without a User-Agent.
            .header(header::USER_AGENT, "lectern-web")
            .header(header::ACCEPT, accept);
        if let Some(token) = &self.token {
            req = req.bearer_auth(token);
        }
        req
    }
}

pub async fn latest_release(State(state): State<AppState>) -> Result<Json<LatestRelease>, AppError> {
    let releases = &state.releases;

    if let Some((at, cached)) = releases.cache.lock().unwrap().clone() {
        if at.elapsed() < CACHE_TTL {
            return Ok(Json(cached));
        }
    }

    let url = format!("https://api.github.com/repos/{}/releases/latest", releases.repo);
    let resp = releases
        .github_get(&url, "application/vnd.github+json")
        .send()
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;
    if !resp.status().is_success() {
        // Private repo without a token, or no release published yet: either
        // way the page falls back to its static GitHub link.
        return Err(AppError::NotFound);
    }

    let release: Value = resp.json().await.map_err(|e| AppError::Internal(e.to_string()))?;
    let view = LatestRelease {
        tag_name: release["tag_name"].as_str().unwrap_or("").to_string(),
        published_at: release["published_at"].as_str().unwrap_or("").to_string(),
        assets: release["assets"]
            .as_array()
            .map(|assets| {
                assets
                    .iter()
                    .filter_map(|a| {
                        Some(AssetView {
                            id: a["id"].as_u64()?,
                            name: a["name"].as_str()?.to_string(),
                            size: a["size"].as_u64().unwrap_or(0),
                        })
                    })
                    .collect()
            })
            .unwrap_or_default(),
    };

    *releases.cache.lock().unwrap() = Some((Instant::now(), view.clone()));
    Ok(Json(view))
}

/// Streams one release asset through to the visitor. GitHub redirects asset
/// downloads to short-lived storage URLs; reqwest follows that redirect and
/// drops the Authorization header across the host change, as required.
pub async fn download_asset(
    State(state): State<AppState>,
    AxumPath((asset_id, name)): AxumPath<(u64, String)>,
) -> Result<Response, AppError> {
    let releases = &state.releases;

    let url = format!(
        "https://api.github.com/repos/{}/releases/assets/{}",
        releases.repo, asset_id
    );
    let resp = releases
        .github_get(&url, "application/octet-stream")
        .send()
        .await
        .map_err(|e| AppError::Internal(e.to_string()))?;
    if !resp.status().is_success() {
        return Err(AppError::NotFound);
    }

    let len = resp.content_length();
    // The name is only cosmetic (the id picks the asset); make it header-safe.
    let safe_name: String = name
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_' | ' ' | '(' | ')'))
        .collect();

    let mut builder = Response::builder()
        .status(StatusCode::OK)
        .header(header::CONTENT_TYPE, "application/octet-stream")
        .header(
            header::CONTENT_DISPOSITION,
            format!("attachment; filename=\"{}\"", safe_name),
        );
    if let Some(len) = len {
        builder = builder.header(header::CONTENT_LENGTH, len);
    }

    builder
        .body(Body::from_stream(resp.bytes_stream()))
        .map_err(|e| AppError::Internal(e.to_string()))
        .map(IntoResponse::into_response)
}
