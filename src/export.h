// Server-side PDF rendering. The client sends the same self-contained HTML
// string it used to feed into `iframe.srcdoc` + `window.print()` (see
// `wrapExportHtml` in web/main.js); instead of relying on the visitor's own
// browser print dialog — whose "Headers and footers" toggle is a browser
// setting no page can control — a headless Chromium instance renders it here
// and calls CDP's Page.printToPDF with headers/footers explicitly disabled.
//
// This HTML is visitor-authored (unsanitized markdown-to-HTML output), and
// rendering it in a real browser process on the server (rather than in the
// visitor's own sandboxed browser) opens up SSRF and script-execution risk
// that didn't exist before. Every tab is locked down before navigating:
// JS execution is disabled outright (the export HTML needs none — mermaid
// diagrams and notebook pages are already pre-rendered to static SVG/PNG
// client-side), and a Fetch-domain interceptor allowlists only `file://`
// (the temp file holding the HTML) and `data:` (already-embedded base64
// images), blocking every other request — including IP-literal SSRF targets
// that `--host-resolver-rules` alone wouldn't catch.
//
// The Rust original drove Chromium through the `headless_chrome` crate. This
// port speaks the DevTools Protocol directly over a WebSocket (drogon's
// WebSocketClient on a dedicated event loop), because no equivalent C++
// library exists and the security properties above are the whole point of
// rendering server-side — degrading them to a plain `--print-to-pdf`
// invocation would lose both the JS lockdown and the request allowlist.
#pragma once

namespace lectern::pdf_export {

/// Starts the dedicated event loop the CDP WebSocket runs on. Call once from
/// main() before serving.
void init();

void register_routes();

}  // namespace lectern::pdf_export
