#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <optional>
#include <string>
#include <string_view>

#include "http.h"

namespace lectern {
namespace auth {

/// Reads the authenticated user's id, preferring an `Authorization: Bearer
/// <api_token>` header over the session cookie. The desktop app authenticates
/// this way because its webview origin can't rely on the session cookie
/// (sessions are SameSite=Strict, which cross-origin requests from a desktop
/// webview won't carry). Browser clients keep using the cookie.
/// Throws AppError::unauthorized() when neither identifies a user.
std::string current_user_id_with_headers(const drogon::HttpRequestPtr &req);

/// Reads the authenticated user's id out of the session cookie only. Kept
/// separate for handlers (like `me` and `export`) that only ever run against
/// a cookie session.
std::string current_user_id(const drogon::HttpRequestPtr &req);

/// Auth that doesn't fail: link-based endpoints work without an account, but
/// still attribute actions to the caller when they happen to be logged in.
std::optional<std::string> optional_user_id(const drogon::HttpRequestPtr &req);

/// A deliberately conservative email check: exactly one `@`, non-empty local
/// and domain parts, a dot in the domain, a sane length, and no characters
/// that don't belong in an address. The frontend HTML-escapes emails wherever
/// it renders them, so this is defense-in-depth — it keeps hostile or
/// malformed values (e.g. ones carrying markup) out of the database entirely
/// rather than relying on every render site to escape.
bool is_valid_email(std::string_view email);

void register_routes();

}  // namespace auth
}  // namespace lectern
