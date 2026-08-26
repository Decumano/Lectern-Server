#include "downloads.h"

#include <cpr/cpr.h>
#include <drogon/HttpAppFramework.h>

#include <cctype>
#include <thread>

#include "error.h"
#include "http.h"
#include "state.h"
#include "util.h"

using json = nlohmann::json;

namespace lectern {

std::shared_ptr<Releases> Releases::from_env()
{
    auto releases = std::make_shared<Releases>();
    releases->repo_ =
        util::env_or("DESKTOP_RELEASES_REPO", "Decumano/OfficeSuite");
    auto token = util::env("GITHUB_RELEASES_TOKEN");
    if (token && !token->empty())
    {
        releases->token_ = std::move(token);
    }
    return releases;
}

std::optional<json> Releases::cached()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cache_)
    {
        return std::nullopt;
    }
    if (std::chrono::steady_clock::now() - cached_at_ >= kCacheTtl)
    {
        return std::nullopt;
    }
    return cache_;
}

void Releases::store(json view)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cache_ = std::move(view);
    cached_at_ = std::chrono::steady_clock::now();
}

namespace downloads {

namespace {

/// An asset larger than this is refused rather than buffered.
///
/// The Rust original streamed GitHub's response straight through to the
/// visitor, so size never mattered. Drogon's async streams are driven from
/// the event loop and cpr's write callback runs on a worker thread, so
/// bridging the two safely would need a hand-written buffer and handshake.
/// Buffering whole is simpler and honest about its bound: desktop installers
/// are single-digit megabytes, and this ceiling is two orders of magnitude
/// above that. Raise it with DOWNLOAD_MAX_BYTES if a release ever needs it.
size_t max_asset_bytes()
{
    static const size_t value = static_cast<size_t>(
        util::env_i64("DOWNLOAD_MAX_BYTES", 256LL * 1024 * 1024));
    return value;
}

cpr::Header github_headers(const Releases &releases, const char *accept)
{
    cpr::Header headers{
        // GitHub rejects requests without a User-Agent.
        {"User-Agent", "lectern-server-cpp"},
        {"Accept", accept}};
    if (releases.token())
    {
        headers["Authorization"] = "Bearer " + *releases.token();
    }
    return headers;
}

/// The name is only cosmetic (the id picks the asset); make it header-safe.
std::string safe_attachment_name(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '.' || c == '-' || c == '_' ||
            c == ' ' || c == '(' || c == ')')
        {
            out.push_back(c);
        }
    }
    return out;
}

/// GitHub work happens on a detached thread: cpr is blocking, and drogon's
/// event loop must not be. Drogon callbacks are safe to invoke from any
/// thread.
template <typename Fn>
void off_thread(HttpCallback callback, Fn &&fn)
{
    std::thread([callback = std::move(callback),
                 fn = std::forward<Fn>(fn)]() mutable {
        http::guard(std::move(callback), [&] { return fn(); });
    }).detach();
}

}  // namespace

void register_routes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/downloads/latest",
        [](const drogon::HttpRequestPtr &, HttpCallback &&callback) {
            off_thread(std::move(callback), []() -> drogon::HttpResponsePtr {
                auto releases = state().releases;
                if (const auto cached = releases->cached())
                {
                    return http::json_response(*cached);
                }

                const std::string url = "https://api.github.com/repos/" +
                                        releases->repo() + "/releases/latest";
                const cpr::Response response = cpr::Get(
                    cpr::Url{url},
                    github_headers(*releases, "application/vnd.github+json"),
                    cpr::Timeout{15000});

                // Private repo without a token, or no release published yet:
                // either way the page falls back to its static GitHub link.
                if (response.status_code < 200 || response.status_code >= 300)
                {
                    throw AppError::not_found();
                }

                const auto release =
                    json::parse(response.text, nullptr, false);
                if (release.is_discarded())
                {
                    throw AppError::internal("malformed GitHub response");
                }

                json assets = json::array();
                if (release.contains("assets") && release["assets"].is_array())
                {
                    for (const auto &asset : release["assets"])
                    {
                        if (!asset.contains("id") ||
                            !asset["id"].is_number_unsigned() ||
                            !asset.contains("name") ||
                            !asset["name"].is_string())
                        {
                            continue;
                        }
                        assets.push_back(
                            {{"id", asset["id"].get<uint64_t>()},
                             {"name", asset["name"].get<std::string>()},
                             {"size",
                              asset.value("size", static_cast<uint64_t>(0))}});
                    }
                }

                json view = {{"tagName", release.value("tag_name", "")},
                             {"publishedAt",
                              release.value("published_at", "")},
                             {"assets", assets}};

                releases->store(view);
                return http::json_response(view);
            });
        },
        {drogon::Get});

    /// Passes one release asset through to the visitor. GitHub redirects
    /// asset downloads to short-lived storage URLs; cpr follows the redirect
    /// and libcurl drops the Authorization header across the host change, as
    /// required.
    app.registerHandler(
        "/api/downloads/asset/{id}/{name}",
        [](const drogon::HttpRequestPtr &,
           HttpCallback &&callback,
           std::string asset_id,
           std::string name) {
            off_thread(
                std::move(callback),
                [asset_id = std::move(asset_id),
                 name = std::move(name)]() -> drogon::HttpResponsePtr {
                    // The id goes into the upstream URL path; digits only, so
                    // it can never rewrite the request.
                    if (asset_id.empty() ||
                        asset_id.find_first_not_of("0123456789") !=
                            std::string::npos)
                    {
                        throw AppError::not_found();
                    }

                    auto releases = state().releases;
                    const std::string url = "https://api.github.com/repos/" +
                                            releases->repo() +
                                            "/releases/assets/" + asset_id;

                    const cpr::Response response = cpr::Get(
                        cpr::Url{url},
                        github_headers(*releases, "application/octet-stream"),
                        cpr::Timeout{0},  // large binaries; no wall clock cap
                        // Follow at most five hops, and never resend the
                        // Authorization header across a host change — which
                        // is exactly what GitHub's redirect to short-lived
                        // storage URLs requires (cont_send_cred defaults to
                        // false).
                        cpr::Redirect{5L});

                    if (response.status_code < 200 ||
                        response.status_code >= 300)
                    {
                        throw AppError::not_found();
                    }
                    if (response.text.size() > max_asset_bytes())
                    {
                        throw AppError::internal(
                            "release asset is larger than this server will "
                            "proxy");
                    }

                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeCodeAndCustomString(
                        drogon::CT_CUSTOM, "application/octet-stream");
                    resp->addHeader("Content-Disposition",
                                    "attachment; filename=\"" +
                                        safe_attachment_name(name) + "\"");
                    resp->setBody(response.text);
                    return resp;
                });
        },
        {drogon::Get});
}

}  // namespace downloads
}  // namespace lectern
