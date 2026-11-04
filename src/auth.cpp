#include "auth.h"

#include <drogon/HttpAppFramework.h>

#include <chrono>
#include <filesystem>
#include <mutex>

#include "error.h"
#include "http.h"
#include "session.h"
#include "state.h"
#include "util.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace lectern::auth {

namespace {

// Per-IP abuse limits: 10 failed logins per 15 minutes blocks password
// brute force without ever counting successful logins (so a shared proxy IP
// doesn't lock out legitimate users); 20 registrations per hour blocks mass
// account creation.
constexpr uint32_t cLoginFailLimit = 10;
constexpr std::chrono::seconds cLoginFailWindow = 15min;
constexpr uint32_t cRegisterLimit = 20;
constexpr std::chrono::seconds cRegisterWindow = 60min;

std::string fGenerateApiToken()
{
    return util::fUuidV4();
}

/// Burns the same time as a real password check, so a login against an
/// unknown email doesn't return measurably faster than one against a wrong
/// password — which would let an attacker probe which emails have accounts.
void fDummyVerify(const std::string &password)
{
    static std::once_flag once;
    static std::string dummy_hash;
    std::call_once(once, [] {
        try
        {
            dummy_hash = util::fHashPassword("dummy-timing-pad");
        }
        catch (const std::exception &)
        {
            dummy_hash.clear();
        }
    });
    (void)util::fVerifyPassword(password, dummy_hash);
}

struct Credentials
{
    std::string email;
    std::string password;
};

Credentials fCredentialsFrom(const drogon::HttpRequestPtr &req)
{
    const auto body = http::fJsonBody(req);
    return {http::fJsonString(body, "email"),
            http::fJsonString(body, "password")};
}

}  // namespace

bool fIsValidEmail(std::string_view email)
{
    if (email.size() < 3 || email.size() > 254)
    {
        return false;
    }

    // No whitespace or angle brackets/quotes that only ever show up in attacks.
    for (const char c : email)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isspace(uc) != 0 || c == '<' || c == '>' || c == '"' ||
            c == '\'' || c == '`' || c == '\\' || c == '\0')
        {
            return false;
        }
    }

    const size_t at = email.find('@');
    if (at == std::string_view::npos)
    {
        return false;
    }
    if (email.find('@', at + 1) != std::string_view::npos)
    {
        return false;  // more than one '@'
    }

    const std::string_view local = email.substr(0, at);
    const std::string_view domain = email.substr(at + 1);

    return !local.empty() && domain.find('.') != std::string_view::npos &&
           domain.front() != '.' && domain.back() != '.';
}

std::string fCurrentUserId(const drogon::HttpRequestPtr &req)
{
    const auto id = session::fCurrentUserId(req);
    if (!id)
    {
        throw AppError::fUnauthorized();
    }
    return *id;
}

std::string fCurrentUserIdWithHeaders(const drogon::HttpRequestPtr &req)
{
    const std::string authorization = req->getHeader("authorization");
    static constexpr std::string_view cBearer = "Bearer ";
    if (util::fStartsWith(authorization, cBearer))
    {
        const std::string token = authorization.substr(cBearer.size());
        const auto row = fState().db->fQueryOne(
            "SELECT id FROM users WHERE api_token = ?", {DbValue(token)});
        if (!row)
        {
            throw AppError::fUnauthorized();
        }
        return row->fText(0);
    }

    return fCurrentUserId(req);
}

std::optional<std::string> fOptionalUserId(const drogon::HttpRequestPtr &req)
{
    try
    {
        return fCurrentUserIdWithHeaders(req);
    }
    catch (const AppError &)
    {
        return std::nullopt;
    }
}

void fRegisterRoutes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/auth/register",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                auto &st = fState();
                const std::string ip = st.fClientIp(req);
                if (st.auth_limiter.fIsLimited(
                        ip, "register", cRegisterLimit, cRegisterWindow))
                {
                    throw AppError::fTooManyRequests();
                }
                st.auth_limiter.fRecord(ip, "register", cRegisterWindow);

                const Credentials creds = fCredentialsFrom(req);
                const std::string email =
                    util::fToLower(util::fTrim(creds.email));
                if (!fIsValidEmail(email))
                {
                    throw AppError::fBadRequest("invalid email");
                }
                if (creds.password.size() < 8)
                {
                    throw AppError::fBadRequest(
                        "password must be at least 8 characters");
                }

                const auto existing = st.db->fQueryOne(
                    "SELECT COUNT(*) FROM users WHERE email = ?",
                    {DbValue(email)});
                if (existing && existing->fI64(0) > 0)
                {
                    throw AppError::fConflict("email already registered");
                }

                const std::string id = util::fUuidV4();
                const std::string password_hash =
                    util::fHashPassword(creds.password);
                const std::string api_token = fGenerateApiToken();

                st.db->fExecute(
                    "INSERT INTO users (id, email, password_hash, created_at,"
                    " api_token) VALUES (?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(email),
                     DbValue(password_hash),
                     DbValue(util::fNowS()),
                     DbValue(api_token)});

                std::error_code ec;
                fs::create_directories(st.workspaces_dir / id, ec);

                auto resp = http::fJsonResponse({{"id", id},
                                                 {"email", email},
                                                 {"apiToken", api_token}});
                resp->addCookie(session::fStart(id));
                return resp;
            });
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/login",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                auto &st = fState();
                const std::string ip = st.fClientIp(req);
                if (st.auth_limiter.fIsLimited(
                        ip, "login-fail", cLoginFailLimit, cLoginFailWindow))
                {
                    throw AppError::fTooManyRequests();
                }

                const Credentials creds = fCredentialsFrom(req);
                const std::string email =
                    util::fToLower(util::fTrim(creds.email));

                const auto row = st.db->fQueryOne(
                    "SELECT id, email, password_hash, api_token FROM users"
                    " WHERE email = ?",
                    {DbValue(email)});

                if (!row)
                {
                    fDummyVerify(creds.password);
                    st.auth_limiter.fRecord(ip, "login-fail", cLoginFailWindow);
                    throw AppError::fUnauthorized();
                }

                const std::string id = row->fText(0);
                const std::string stored_email = row->fText(1);
                if (!util::fVerifyPassword(creds.password, row->fText(2)))
                {
                    st.auth_limiter.fRecord(ip, "login-fail", cLoginFailWindow);
                    throw AppError::fUnauthorized();
                }

                // Users created before the api_token column existed have ''
                // here; mint one lazily so every account ends up with a token
                // without a backfill script.
                std::string api_token = row->fText(3);
                if (api_token.empty())
                {
                    api_token = fGenerateApiToken();
                    st.db->fExecute(
                        "UPDATE users SET api_token = ? WHERE id = ?",
                        {DbValue(api_token), DbValue(id)});
                }

                auto resp = http::fJsonResponse({{"id", id},
                                                 {"email", stored_email},
                                                 {"apiToken", api_token}});
                resp->addCookie(session::fStart(id));
                return resp;
            });
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/logout",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                auto resp = http::fOkResponse();
                resp->addCookie(session::fDestroy(req));
                return resp;
            });
        },
        {drogon::Post});

    // Identity without the API token. `/me` is reachable with nothing but the
    // session cookie, so returning the long-lived bearer token here would let
    // any script running on the page trade same-origin access for a
    // permanent, non-expiring credential. The desktop app receives its token
    // from login/register, which require the password, and doesn't call this.
    app.registerHandler(
        "/api/auth/me",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                const std::string id = fCurrentUserIdWithHeaders(req);
                const auto row = fState().db->fQueryOne(
                    "SELECT id, email FROM users WHERE id = ?", {DbValue(id)});
                if (!row)
                {
                    throw AppError::fUnauthorized();
                }
                return http::fJsonResponse(
                    {{"id", row->fText(0)}, {"email", row->fText(1)}});
            });
        },
        {drogon::Get});
}

}  // namespace lectern::auth
