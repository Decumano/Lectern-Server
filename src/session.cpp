#include "session.h"

#include <sodium.h>

#include <array>

#include "state.h"
#include "util.h"

namespace lectern::session {

namespace {

constexpr int64_t kInactivitySeconds =
    static_cast<int64_t>(kInactivityDays) * 24 * 60 * 60;

/// HMAC-SHA256 of the session id under SESSION_SECRET, hex-encoded.
std::string sign(const std::string &session_id)
{
    const std::string &secret = state().session_secret;
    std::array<unsigned char, crypto_auth_hmacsha256_BYTES> mac{};

    // The secret is arbitrary-length text; HMAC wants a fixed-size key, so
    // hash it down first (standard practice, and what tower-sessions' Key
    // derivation amounts to for our purposes).
    std::array<unsigned char, crypto_hash_sha256_BYTES> key{};
    crypto_hash_sha256(key.data(),
                       reinterpret_cast<const unsigned char *>(secret.data()),
                       secret.size());

    crypto_auth_hmacsha256(
        mac.data(),
        reinterpret_cast<const unsigned char *>(session_id.data()),
        session_id.size(),
        key.data());

    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(mac.size() * 2);
    for (unsigned char byte : mac)
    {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

/// Extracts the session id from the cookie, verifying the signature when one
/// is configured. Returns nullopt for a missing, malformed or forged cookie.
std::optional<std::string> id_from_cookie(const drogon::HttpRequestPtr &req)
{
    const std::string raw = req->getCookie(kCookieName);
    if (raw.empty())
    {
        return std::nullopt;
    }

    const bool signed_cookies = !state().session_secret.empty();
    const size_t dot = raw.find('.');

    if (!signed_cookies)
    {
        // Unsigned mode: tolerate a value that still carries a signature from
        // before the secret was removed, but ignore it.
        return dot == std::string::npos ? raw : raw.substr(0, dot);
    }

    if (dot == std::string::npos)
    {
        return std::nullopt;  // signing is on; an unsigned cookie is invalid
    }

    const std::string id = raw.substr(0, dot);
    const std::string mac = raw.substr(dot + 1);
    if (id.empty() || !util::constant_time_equals(mac, sign(id)))
    {
        return std::nullopt;
    }
    return id;
}

drogon::Cookie make_cookie(const std::string &value, int max_age_seconds)
{
    drogon::Cookie cookie(kCookieName, value);
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSecure(state().cookie_secure);
    // Strict is what tower-sessions defaulted to, and what the desktop app's
    // bearer-token path exists to work around — see auth.cpp.
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    cookie.setMaxAge(max_age_seconds);
    return cookie;
}

}  // namespace

std::optional<std::string> current_user_id(const drogon::HttpRequestPtr &req)
{
    const auto id = id_from_cookie(req);
    if (!id)
    {
        return std::nullopt;
    }

    auto &db = *state().db;
    const auto row = db.query_one(
        "SELECT user_id, expires_at FROM sessions WHERE id = ?",
        {DbValue(*id)});
    if (!row)
    {
        return std::nullopt;
    }

    const int64_t now = util::now_s();
    if (row->i64(1) <= now)
    {
        db.execute("DELETE FROM sessions WHERE id = ?", {DbValue(*id)});
        return std::nullopt;
    }

    // Sliding expiry, matching Expiry::OnInactivity in the Rust server.
    db.execute("UPDATE sessions SET expires_at = ? WHERE id = ?",
               {DbValue(now + kInactivitySeconds), DbValue(*id)});

    return row->text(0);
}

drogon::Cookie start(const std::string &user_id)
{
    const std::string id = util::random_hex(32);

    state().db->execute(
        "INSERT INTO sessions (id, user_id, expires_at) VALUES (?, ?, ?)",
        {DbValue(id),
         DbValue(user_id),
         DbValue(util::now_s() + kInactivitySeconds)});

    const std::string value =
        state().session_secret.empty() ? id : id + "." + sign(id);
    return make_cookie(value, static_cast<int>(kInactivitySeconds));
}

drogon::Cookie destroy(const drogon::HttpRequestPtr &req)
{
    if (const auto id = id_from_cookie(req))
    {
        state().db->execute("DELETE FROM sessions WHERE id = ?",
                            {DbValue(*id)});
    }
    return make_cookie("", 0);
}

void purge_expired()
{
    state().db->execute("DELETE FROM sessions WHERE expires_at <= ?",
                        {DbValue(util::now_s())});
}

}  // namespace lectern::session
