#include <drogon/HttpAppFramework.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

#include "auth.h"
#include "downloads.h"
#include "export.h"
#include "fonts.h"
#include "session.h"
#include "share.h"
#include "state.h"
#include "util.h"
#include "workspace.h"

namespace fs = std::filesystem;
using namespace lectern;

namespace {

/// Cache policy for the static frontend. Browsers heuristically cache
/// responses that carry no Cache-Control at all (10% of file age), which
/// meant a deployed update could serve a stale index.html — and with it,
/// stale everything — for days. HTML entry points must always revalidate
/// (`no-cache` still allows 304s, so it stays cheap); other assets get an
/// hour, and index.html's `?v=N` query params handle hard busts on deploys.
void install_static_cache_policy()
{
    // PreSending rather than PostHandling: static files are served by
    // drogon's own file handler, which never runs the post-handling advice,
    // and index.html is precisely the response this policy exists for.
    drogon::app().registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr &req,
           const drogon::HttpResponsePtr &resp) {
            const std::string path(req->path());
            if (util::starts_with(path, "/api"))
            {
                return;
            }
            const bool is_html = path == "/" || util::ends_with(path, ".html") ||
                                 path.find('.') == std::string::npos;
            // addHeader would append a second value if drogon's static file
            // handler already set one.
            resp->removeHeader("Cache-Control");
            resp->addHeader("Cache-Control",
                            is_html ? "no-cache"
                                    : "public, max-age=3600, must-revalidate");
        });
}

/// Single-page fallback: any unmatched, non-API path serves index.html so the
/// frontend's client-side routes survive a hard refresh. `set404 = false`
/// keeps the status at 200, which is what the Rust `ServeDir::fallback` did.
void install_spa_fallback(const fs::path &web_root)
{
    const fs::path index = web_root / "index.html";
    const auto html = util::read_file(index);
    if (!html)
    {
        spdlog::warn(
            "no {} found — the frontend submodule is probably not checked "
            "out (git submodule update --init)",
            index.string());
        return;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCode(drogon::CT_TEXT_HTML);
    resp->setBody(*html);
    drogon::app().setCustom404Page(resp, /*set404=*/false);
}

}  // namespace

int main()
{
    try
    {
        util::load_dotenv();
        util::init_crypto();

        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        spdlog::set_level(
            spdlog::level::from_str(util::env_or("LOG_LEVEL", "info")));

        auto &st = state();

        // Everything this server persists lives under one root, so a
        // deployment can move its state to another disk with a single
        // setting. DATABASE_URL, WORKSPACES_DIR and FONTS_DIR still win when
        // set explicitly — they only fall back to DATA_DIR — which keeps
        // existing .env files, and the paths baked into older images,
        // working unchanged.
        const fs::path data_dir = fs::path(util::env_or("DATA_DIR", "data"));

        const std::string database_url = util::env_or(
            "DATABASE_URL", "sqlite://" + (data_dir / "officesuite.db").generic_string());
        st.workspaces_dir = fs::path(
            util::env_or("WORKSPACES_DIR", (data_dir / "workspaces").string()));
        st.fonts_dir =
            fs::path(util::env_or("FONTS_DIR", (data_dir / "fonts").string()));
        const auto port =
            static_cast<uint16_t>(util::env_i64("PORT", 8080));
        const fs::path web_root = fs::path(util::env_or("WEB_DIR", "web"));
        const fs::path migrations_dir =
            fs::path(util::env_or("MIGRATIONS_DIR", "migrations"));

        std::error_code ec;
        fs::create_directories(st.workspaces_dir, ec);
        fs::create_directories(st.fonts_dir, ec);

        st.db = std::make_shared<Db>(database_url);
        st.db->run_migrations(migrations_dir);

        // Session cookies are only marked Secure when COOKIE_SECURE=true; set
        // it once the server sits behind HTTPS (Secure cookies never arrive
        // over plain http://, which would break local development).
        st.cookie_secure = util::env_bool("COOKIE_SECURE", false);

        // SESSION_SECRET signs the session cookie so a tampered or forged
        // cookie is rejected outright. Optional for local development
        // (unsigned cookies plus a warning), strongly recommended for any
        // real deployment.
        st.session_secret = util::env_or("SESSION_SECRET", "");
        if (st.session_secret.empty())
        {
            spdlog::warn(
                "SESSION_SECRET is not set; session cookies are unsigned "
                "(fine for local dev)");
        }
        else if (st.session_secret.size() < 64)
        {
            spdlog::error(
                "SESSION_SECRET must be at least 64 bytes of random data");
            return 1;
        }

        // Reverse-proxy peers whose X-Forwarded-For is trusted for rate-limit
        // bucketing, e.g. TRUSTED_PROXIES=127.0.0.1,::1
        for (const auto &entry :
             util::split(util::env_or("TRUSTED_PROXIES", ""), ','))
        {
            const std::string trimmed = util::trim(entry);
            if (!trimmed.empty())
            {
                st.trusted_proxies.insert(trimmed);
            }
        }

        // Per-account workspace ceiling; registration is open, so without a
        // cap any account could fill the disk. 0 disables the check.
        st.workspace_quota_bytes = static_cast<uint64_t>(util::env_i64(
            "WORKSPACE_QUOTA_BYTES", 256LL * 1024 * 1024));

        st.releases = Releases::from_env();

        auth::register_routes();
        workspace::register_routes();
        share::register_routes();
        fonts::register_routes();
        downloads::register_routes();
        pdf_export::init();
        pdf_export::register_routes();

        install_static_cache_policy();
        install_spa_fallback(web_root);

        // Expired session rows would otherwise accumulate forever.
        drogon::app().getLoop()->runEvery(60 * 60.0, [] {
            try
            {
                session::purge_expired();
            }
            catch (const std::exception &error)
            {
                spdlog::warn("session purge failed: {}", error.what());
            }
        });

        // No CORS layer on purpose: the browser frontend is served
        // same-origin from this process, and the desktop app calls the API
        // from native code, which isn't subject to CORS. Not exposing
        // cross-origin access is the safer default for a self-hosted
        // instance.
        // Drogon serves only an allowlist of extensions, and its default omits
        // several the frontend ships. `.mjs` is the one that matters:
        // index.html imports pdf.js as an ES module, and a 404 there fell
        // through to the SPA fallback, which answered with index.html — so the
        // browser rejected it on MIME type and PDF import and annotation
        // silently stopped working. tower-http's ServeDir, which the Rust
        // build uses, has no such allowlist, so this gap was a port artifact.
        drogon::app().setFileTypes({
            "html", "htm", "js", "mjs", "css", "map",
            "json", "txt", "md", "xml", "svg",
            "png", "jpg", "jpeg", "gif", "webp", "bmp", "ico", "icns",
            "ttf", "otf", "woff", "woff2", "eot",
            "wasm", "pdf",
        });

        drogon::app()
            .setDocumentRoot(web_root.string())
            // No handler here parses multipart bodies, but drogon creates its
            // upload scratch tree at startup regardless — in the working
            // directory, which left an `uploads/` beside the binary. Keeping
            // it under DATA_DIR means every directory this process creates
            // lives in the one place an operator configured.
            .setUploadPath((data_dir / "uploads").string())
            .setLogLevel(trantor::Logger::kWarn)
            // Every handler does blocking filesystem and SQLite work (see
            // db.h); the pool is what keeps one slow write from stalling
            // unrelated requests.
            .setThreadNum(static_cast<size_t>(util::env_i64("THREADS", 8)))
            // Drogon's limit is global, so it has to clear the largest of
            // the per-route ceilings the handlers enforce: PDF export bodies,
            // and MAX_WORK_FILE_BYTES for workspace writes (workspace.cpp;
            // fonts.cpp caps fonts at 5MB).
            .setClientMaxBodySize(static_cast<size_t>(std::max<int64_t>(
                50LL * 1024 * 1024,
                util::env_i64("MAX_WORK_FILE_BYTES", 32LL * 1024 * 1024) +
                    1024 * 1024)))
            .addListener("0.0.0.0", port);

        spdlog::info("lectern-server listening on http://0.0.0.0:{}", port);
        // Worth one line at startup: a mistyped DATA_DIR otherwise looks like
        // an empty instance rather than a misconfigured one.
        spdlog::info("storing workspaces in {} and fonts in {}",
                     fs::absolute(st.workspaces_dir).string(),
                     fs::absolute(st.fonts_dir).string());
        drogon::app().run();
        return 0;
    }
    catch (const std::exception &error)
    {
        spdlog::critical("startup failed: {}", error.what());
        return 1;
    }
}
