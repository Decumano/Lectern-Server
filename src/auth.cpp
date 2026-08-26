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
constexpr uint32_t kLoginFailLimit = 10;
constexpr std::chrono::seconds kLoginFailWindow = 15min;
constexpr uint32_t kRegisterLimit = 20;
constexpr std::chrono::seconds kRegisterWindow = 60min;

std::string generate_api_token()
{
    return util::uuid_v4();
}

/// Burns the same time as a real password check, so a login against an
/// unknown email doesn't return measurably faster than one against a wrong
/// password — which would let an attacker probe which emails have accounts.
void dummy_verify(const std::string &password)
{
    static std::once_flag once;
    static std::string dummy_hash;
    std::call_once(once, [] {
        try
        {
            dummy_hash = util::hash_password("dummy-timing-pad");
        }
        catch (const std::exception &)
        {
            dummy_hash.clear();
        }
    });
    (void)util::verify_password(password, dummy_hash);
}

struct Credentials
{
    std::string email;
    std::string password;
};

Credentials credentials_from(const drogon::HttpRequestPtr &req)
{
    const auto body = http::json_body(req);
    return {http::json_string(body, "email"),
            http::json_string(body, "password")};
}

}  // namespace

bool is_valid_email(std::string_view email)
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

std::string current_user_id(const drogon::HttpRequestPtr &req)
{
    const auto id = session::current_user_id(req);
    if (!id)
    {
        throw AppError::unauthorized();
    }
    return *id;
}

std::string current_user_id_with_headers(const drogon::HttpRequestPtr &req)
{
    const std::string authorization = req->getHeader("authorization");
    static constexpr std::string_view kBearer = "Bearer ";
    if (util::starts_with(authorization, kBearer))
    {
        const std::string token = authorization.substr(kBearer.size());
        const auto row = state().db->query_one(
            "SELECT id FROM users WHERE api_token = ?", {DbValue(token)});
        if (!row)
        {
            throw AppError::unauthorized();
        }
        return row->text(0);
    }

    return current_user_id(req);
}

std::optional<std::string> optional_user_id(const drogon::HttpRequestPtr &req)
{
    try
    {
        return current_user_id_with_headers(req);
    }
    catch (const AppError &)
    {
        return std::nullopt;
    }
}

void register_routes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/auth/register",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                auto &st = state();
                const std::string ip = st.client_ip(req);
                if (st.auth_limiter.is_limited(
                        ip, "register", kRegisterLimit, kRegisterWindow))
                {
                    throw AppError::too_many_requests();
                }
                st.auth_limiter.record(ip, "register", kRegisterWindow);

                const Credentials creds = credentials_from(req);
                const std::string email =
                    util::to_lower(util::trim(creds.email));
                if (!is_valid_email(email))
                {
                    throw AppError::bad_request("invalid email");
                }
                if (creds.password.size() < 8)
                {
                    throw AppError::bad_request(
                        "password must be at least 8 characters");
                }

                const auto existing = st.db->query_one(
                    "SELECT COUNT(*) FROM users WHERE email = ?",
                    {DbValue(email)});
                if (existing && existing->i64(0) > 0)
                {
                    throw AppError::conflict("email already registered");
                }

                const std::string id = util::uuid_v4();
                const std::string password_hash =
                    util::hash_password(creds.password);
                const std::string api_token = generate_api_token();

                st.db->execute(
                    "INSERT INTO users (id, email, password_hash, created_at,"
                    " api_token) VALUES (?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(email),
                     DbValue(password_hash),
                     DbValue(util::now_s()),
                     DbValue(api_token)});

                std::error_code ec;
                fs::create_directories(st.workspaces_dir / id, ec);

                auto resp = http::json_response({{"id", id},
                                                 {"email", email},
                                                 {"apiToken", api_token}});
                resp->addCookie(session::start(id));
                return resp;
            });
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/login",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                auto &st = state();
                const std::string ip = st.client_ip(req);
                if (st.auth_limiter.is_limited(
                        ip, "login-fail", kLoginFailLimit, kLoginFailWindow))
                {
                    throw AppError::too_many_requests();
                }

                const Credentials creds = credentials_from(req);
                const std::string email =
                    util::to_lower(util::trim(creds.email));

                const auto row = st.db->query_one(
                    "SELECT id, email, password_hash, api_token FROM users"
                    " WHERE email = ?",
                    {DbValue(email)});

                if (!row)
                {
                    dummy_verify(creds.password);
                    st.auth_limiter.record(ip, "login-fail", kLoginFailWindow);
                    throw AppError::unauthorized();
                }

                const std::string id = row->text(0);
                const std::string stored_email = row->text(1);
                if (!util::verify_password(creds.password, row->text(2)))
                {
                    st.auth_limiter.record(ip, "login-fail", kLoginFailWindow);
                    throw AppError::unauthorized();
                }

                // Users created before the api_token column existed have ''
                // here; mint one lazily so every account ends up with a token
                // without a backfill script.
                std::string api_token = row->text(3);
                if (api_token.empty())
                {
                    api_token = generate_api_token();
                    st.db->execute(
                        "UPDATE users SET api_token = ? WHERE id = ?",
                        {DbValue(api_token), DbValue(id)});
                }

                auto resp = http::json_response({{"id", id},
                                                 {"email", stored_email},
                                                 {"apiToken", api_token}});
                resp->addCookie(session::start(id));
                return resp;
            });
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/logout",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                auto resp = http::ok_response();
                resp->addCookie(session::destroy(req));
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
            http::guard(std::move(callback), [&] {
                const std::string id = current_user_id_with_headers(req);
                const auto row = state().db->query_one(
                    "SELECT id, email FROM users WHERE id = ?", {DbValue(id)});
                if (!row)
                {
                    throw AppError::unauthorized();
                }
                return http::json_response(
                    {{"id", row->text(0)}, {"email", row->text(1)}});
            });
        },
        {drogon::Get});
}

}  // namespace lectern::auth
