// Cookie sessions backed by the `sessions` table.
//
// The Rust server used tower-sessions with a SQLite store. Drogon ships its
// own session support, but it lives in process memory: restarting the server
// would sign every user out, and two workers behind a load balancer would
// disagree about who is logged in. So this module reimplements the same
// contract the Rust server offered — persistent, signed, 30-day-inactivity
// cookies — against a table this codebase owns (migrations/0008_sessions.sql).
//
// Cookie value is `<session-id>` when SESSION_SECRET is unset (a warning is
// logged; local development only) and `<session-id>.<hmac-sha256-hex>` when it
// is set, so a forged or tampered id is rejected before it ever reaches the
// database.
#pragma once

#include <drogon/Cookie.h>
#include <drogon/HttpRequest.h>

#include <optional>
#include <string>

namespace lectern::session {

/// Cookie name. Deliberately distinct from drogon's own JSESSIONID so the
/// framework's session machinery, if it is ever enabled, cannot collide.
inline constexpr const char *kCookieName = "lectern_session";

/// Sessions expire this long after they were last used.
inline constexpr int kInactivityDays = 30;

/// The user id of the caller's valid session, or nullopt. Sliding expiry:
/// a successful read pushes the session's expiry back out to the full
/// inactivity window.
std::optional<std::string> current_user_id(const drogon::HttpRequestPtr &req);

/// Creates a session for `user_id` and returns the cookie that carries it.
/// The caller attaches it to the response.
drogon::Cookie start(const std::string &user_id);

/// Deletes the caller's session row and returns an already-expired cookie
/// that clears it from the browser.
drogon::Cookie destroy(const drogon::HttpRequestPtr &req);

/// Drops rows whose expiry has passed. Called periodically from main().
void purge_expired();

}  // namespace lectern::session
