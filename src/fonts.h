// Account-level custom fonts. Solves two problems that OS/browser-level fonts
// can't: the live editor's font picker depends on the browser's Local Font
// Access API, which only exists in a secure context (HTTPS, or literally
// "localhost") and only in Chromium — so it silently falls back to a generic
// list on any plain-HTTP deployment. And PDF export renders in a
// headless-Chromium sandbox that blocks every network request except file://
// and data: (deliberately, to close off SSRF) — so a font installed on the
// visitor's machine, or even the server's own OS, can never reach that
// renderer by name alone.
//
// Fonts uploaded here sidestep both: they're served as real HTTP assets for
// @font-face in the live app (works in any browser, any origin), and
// base64-embedded directly into the exported HTML's <style> for PDF export
// (satisfies the sandbox's data:-only policy with no loosening required).
#pragma once

#include <cstddef>
#include <string>

namespace lectern::fonts {

constexpr size_t kMaxFontBytes = 5 * 1024 * 1024;  // 5MB per font file

/// Builds a `<style>` block declaring every one of the user's custom fonts as
/// `@font-face` with the file base64-embedded as a `data:` URI. Used by
/// export.cpp, whose headless-Chromium sandbox refuses any request that isn't
/// `file://` or `data:` — an HTTP `url()` back to this server would simply be
/// dropped, so the bytes have to already be inline before Chromium sees them.
std::string embed_account_fonts_style(const std::string &owner_id);

void register_routes();

}  // namespace lectern::fonts
