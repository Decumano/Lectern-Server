// Server-side PDF rendering. The client sends the same self-contained HTML
// string it used to feed into `iframe.srcdoc` + `window.print()` (see
// `wrapExportHtml` in web/main.js); instead of relying on the visitor's own
// browser print dialog - whose "Headers and footers" toggle is a browser
// setting no page can control - a headless Chromium instance renders it here
// and calls CDP's Page.printToPDF with headers/footers explicitly disabled.
//
// This HTML is visitor-authored (unsanitized markdown-to-HTML output), and
// rendering it in a real browser process on the server (rather than in the
// visitor's own sandboxed browser) opens up SSRF and script-execution risk
// that didn't exist before. Every tab is locked down before navigating:
// JS execution is disabled outright (the export HTML needs none - mermaid
// diagrams and notebook pages are already pre-rendered to static SVG/PNG
// client-side), and a Fetch-domain interceptor allowlists only `file://`
// (the temp file holding the HTML) and `data:` (already-embedded base64
// images), blocking every other request - including IP-literal SSRF targets
// that `--host-resolver-rules` alone wouldn't catch.

use axum::extract::State;
use axum::http::{header, StatusCode};
use axum::response::{IntoResponse, Response};
use headless_chrome::browser::tab::RequestPausedDecision;
use headless_chrome::protocol::cdp::Emulation::SetScriptExecutionDisabled;
use headless_chrome::protocol::cdp::Fetch::{events::RequestPausedEvent, FailRequest};
use headless_chrome::protocol::cdp::Network::ErrorReason;
use headless_chrome::types::PrintToPdfOptions;
use headless_chrome::{Browser, LaunchOptionsBuilder};
use std::ffi::OsStr;
use std::io::Write;
use std::sync::Arc;
use std::time::Duration;
use tower_sessions::Session;

use crate::auth::current_user_id;
use crate::error::AppError;
use crate::fonts::embed_account_fonts_style;
use crate::state::AppState;

const RENDER_TIMEOUT: Duration = Duration::from_secs(30);

pub async fn export_pdf(
    State(state): State<AppState>,
    session: Session,
    html: String,
) -> Result<Response, AppError> {
    let user_id = current_user_id(&session).await?;

    // The renderer below only ever accepts file:// and data: requests (see
    // module doc comment), so any custom fonts the account uploaded have to
    // already be inline as data: URIs before Chromium sees the page — an
    // http:// url() back to this server would just be dropped.
    let font_style = embed_account_fonts_style(&state, &user_id).await;
    let html = match html.find("<head>") {
        Some(pos) => {
            let insert_at = pos + "<head>".len();
            let mut out = String::with_capacity(html.len() + font_style.len());
            out.push_str(&html[..insert_at]);
            out.push_str(&font_style);
            out.push_str(&html[insert_at..]);
            out
        }
        None => format!("{font_style}{html}"),
    };

    let pdf_bytes = tokio::time::timeout(
        RENDER_TIMEOUT,
        tokio::task::spawn_blocking(move || render_pdf(&html)),
    )
    .await
    .map_err(|_| AppError::Internal("PDF render timed out".to_string()))?
    .map_err(|e| AppError::Internal(e.to_string()))?
    .map_err(|e| AppError::Internal(e.to_string()))?;

    Ok((
        StatusCode::OK,
        [(header::CONTENT_TYPE, "application/pdf".to_string())],
        pdf_bytes,
    )
        .into_response())
}

fn render_pdf(html: &str) -> anyhow::Result<Vec<u8>> {
    let mut file = tempfile::Builder::new().suffix(".html").tempfile()?;
    file.write_all(html.as_bytes())?;
    let file_url = format!("file://{}", file.path().display());

    let browser = Browser::new(
        LaunchOptionsBuilder::default()
            .headless(true)
            .sandbox(false)
            .args(vec![OsStr::new("--host-resolver-rules=MAP * 0.0.0.0")])
            .build()?,
    )?;
    let tab = browser.new_tab()?;

    tab.call_method(SetScriptExecutionDisabled { value: true })?;

    tab.enable_fetch(None, None)?;
    tab.enable_request_interception(Arc::new(
        |_transport, _session_id, event: RequestPausedEvent| {
            let url = &event.params.request.url;
            if url.starts_with("file://") || url.starts_with("data:") {
                RequestPausedDecision::Continue(None)
            } else {
                RequestPausedDecision::Fail(FailRequest {
                    request_id: event.params.request_id.clone(),
                    error_reason: ErrorReason::BlockedByClient,
                })
            }
        },
    ))?;

    tab.navigate_to(&file_url)?.wait_until_navigated()?;

    let pdf = tab.print_to_pdf(Some(PrintToPdfOptions {
        display_header_footer: Some(false),
        print_background: Some(true),
        prefer_css_page_size: Some(true),
        ..Default::default()
    }))?;

    Ok(pdf)
}
