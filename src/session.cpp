#include "session.h"

#include <sodium.h>

#include <array>

#include "state.h"
#include "util.h"

namespace lectern::session {

namespace {

constexpr int64_t cInactivitySeconds =
    static_cast<int64_t>(cInactivityDays) * 24 * 60 * 60;

/// HMAC-SHA256 of the session id under SESSION_SECRET, hex-encoded.
std::string fSign(const std::string &session_id)
{
    const std::string &secret = fState().session_secret;
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
std::optional<std::string> fIdFromCookie(const drogon::HttpRequestPtr &req)
{
    const std::string raw = req->getCookie(cCookieName);
    if (raw.empty())
    {
        return std::nullopt;
    }

    const bool signed_cookies = !fState().session_secret.empty();
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
    if (id.empty() || !util::fConstantTimeEquals(mac, fSign(id)))
    {
        return std::nullopt;
    }
    return id;
}

drogon::Cookie fMakeCookie(const std::string &value, int max_age_seconds)
{
    drogon::Cookie cookie(cCookieName, value);
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSecure(fState().cookie_secure);
    // Strict is what tower-sessions defaulted to, and what the desktop app's
    // bearer-token path exists to work around — see auth.cpp.
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    cookie.setMaxAge(max_age_seconds);
    return cookie;
}

}  // namespace

std::optional<std::string> fCurrentUserId(const drogon::HttpRequestPtr &req)
{
    const auto id = fIdFromCookie(req);
    if (!id)
    {
        return std::nullopt;
    }

    auto &db = *fState().db;
    const auto row = db.fQueryOne(
        "SELECT user_id, expires_at FROM sessions WHERE id = ?",
        {DbValue(*id)});
    if (!row)
    {
        return std::nullopt;
    }

    const int64_t now = util::fNowS();
    if (row->fI64(1) <= now)
    {
        db.fExecute("DELETE FROM sessions WHERE id = ?", {DbValue(*id)});
        return std::nullopt;
    }

    // Sliding expiry, matching Expiry::OnInactivity in the Rust server.
    db.fExecute("UPDATE sessions SET expires_at = ? WHERE id = ?",
               {DbValue(now + cInactivitySeconds), DbValue(*id)});

    return row->fText(0);
}

drogon::Cookie fStart(const std::string &user_id)
{
    const std::string id = util::fRandomHex(32);

    fState().db->fExecute(
        "INSERT INTO sessions (id, user_id, expires_at) VALUES (?, ?, ?)",
        {DbValue(id),
         DbValue(user_id),
         DbValue(util::fNowS() + cInactivitySeconds)});

    const std::string value =
        fState().session_secret.empty() ? id : id + "." + fSign(id);
    return fMakeCookie(value, static_cast<int>(cInactivitySeconds));
}

drogon::Cookie fDestroy(const drogon::HttpRequestPtr &req)
{
    if (const auto id = fIdFromCookie(req))
    {
        fState().db->fExecute("DELETE FROM sessions WHERE id = ?",
                            {DbValue(*id)});
    }
    return fMakeCookie("", 0);
}

void fPurgeExpired()
{
    fState().db->fExecute("DELETE FROM sessions WHERE expires_at <= ?",
                        {DbValue(util::fNowS())});
}

}  // namespace lectern::session
