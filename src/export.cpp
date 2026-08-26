#include "export.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/WebSocketClient.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <semaphore>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "auth.h"
#include "error.h"
#include "fonts.h"
#include "http.h"
#include "state.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace lectern::pdf_export {

namespace {

constexpr auto kRenderTimeout = 30s;

/// Each render launches a whole Chromium process, so unbounded concurrency is
/// a trivial way to exhaust the server's RAM and CPU. Requests queue for one
/// of a few slots instead, and give up rather than pile up if the queue
/// doesn't clear.
constexpr int kMaxConcurrentRenders = 2;
constexpr auto kQueueTimeout = 60s;

std::counting_semaphore<kMaxConcurrentRenders> &render_slots()
{
    static std::counting_semaphore<kMaxConcurrentRenders> slots(
        kMaxConcurrentRenders);
    return slots;
}

/// The CDP WebSocket lives on its own event loop so a render thread can block
/// on a reply without deadlocking the loop that is delivering it.
trantor::EventLoopThread &cdp_loop()
{
    static trantor::EventLoopThread loop_thread("lectern-cdp");
    return loop_thread;
}

// ── Locating and running Chromium ──

std::string find_browser()
{
    if (const auto configured = util::env("CHROME_PATH"))
    {
        if (!configured->empty())
        {
            return *configured;
        }
    }

    static const std::vector<std::string> kCandidates = {
#ifdef _WIN32
        // Chrome only. Microsoft Edge is deliberately not listed: its
        // DevTools port binds and shows as LISTENING, but Windows'
        // AppContainer isolation refuses loopback connections to it from
        // another process, so every attach attempt fails with "connection
        // refused" after a stuck SYN_RECEIVED. Falling back to Edge would
        // turn a clear "no browser found" into a confusing timeout.
        "C:/Program Files/Google/Chrome/Application/chrome.exe",
        "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
        "C:/Program Files/Chromium/Application/chrome.exe",
#elif defined(__APPLE__)
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
#else
        "/usr/bin/google-chrome",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
        "/usr/bin/google-chrome-stable",
        "/snap/bin/chromium",
#endif
    };

    for (const auto &candidate : kCandidates)
    {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec)
        {
            return candidate;
        }
    }

    throw AppError::internal(
        "no Chromium found for PDF export; set CHROME_PATH");
}

/// A spawned browser, killed when this object dies so a failed render can
/// never leak a Chromium process.
class ChildProcess
{
  public:
    ChildProcess(const std::string &executable,
                 const std::vector<std::string> &args)
    {
#ifdef _WIN32
        std::string command = quote(executable);
        for (const auto &arg : args)
        {
            command += " " + quote(arg);
        }

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};

        // CreateProcess mutates the command-line buffer, so it cannot be a
        // string literal or a const buffer.
        std::vector<char> buffer(command.begin(), command.end());
        buffer.push_back('\0');

        if (CreateProcessA(nullptr,
                           buffer.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           CREATE_NO_WINDOW,
                           nullptr,
                           nullptr,
                           &startup,
                           &info) == 0)
        {
            throw AppError::internal("failed to start the PDF renderer");
        }
        CloseHandle(info.hThread);
        handle_ = info.hProcess;
#else
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(executable);
        storage.insert(storage.end(), args.begin(), args.end());

        std::vector<char *> argv;
        argv.reserve(storage.size() + 1);
        for (auto &item : storage)
        {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);

        if (posix_spawn(&pid_,
                        executable.c_str(),
                        nullptr,
                        nullptr,
                        argv.data(),
                        environ) != 0)
        {
            pid_ = -1;
            throw AppError::internal("failed to start the PDF renderer");
        }
#endif
    }

    ~ChildProcess()
    {
        kill();
    }

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    void kill()
    {
#ifdef _WIN32
        if (handle_ != nullptr)
        {
            TerminateProcess(handle_, 0);
            WaitForSingleObject(handle_, 5000);
            CloseHandle(handle_);
            handle_ = nullptr;
        }
#else
        if (pid_ > 0)
        {
            ::kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
#endif
    }

  private:
#ifdef _WIN32
    static std::string quote(const std::string &value)
    {
        return "\"" + value + "\"";
    }

    HANDLE handle_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

// ── A blocking DevTools Protocol client ──

class CdpSession
{
  public:
    /// Connects to `ws://127.0.0.1:<port><path>`, giving up after `timeout`.
    void connect(uint16_t port, const std::string &path, std::chrono::seconds timeout)
    {
        client_ = drogon::WebSocketClient::newWebSocketClient(
            "127.0.0.1", port, false, cdp_loop().getLoop());

        client_->setMessageHandler(
            [this](std::string &&message,
                   const drogon::WebSocketClientPtr &,
                   const drogon::WebSocketMessageType &type) {
                if (type == drogon::WebSocketMessageType::Text)
                {
                    on_message(message);
                }
            });

        client_->setConnectionClosedHandler(
            [this](const drogon::WebSocketClientPtr &) {
                std::lock_guard<std::mutex> lock(mutex_);
                closed_ = true;
                condition_.notify_all();
            });

        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath(path);

        std::promise<bool> connected;
        auto connected_future = connected.get_future();
        client_->connectToServer(
            request,
            [&connected](drogon::ReqResult result,
                         const drogon::HttpResponsePtr &,
                         const drogon::WebSocketClientPtr &) {
                connected.set_value(result == drogon::ReqResult::Ok);
            });

        if (connected_future.wait_for(timeout) != std::future_status::ready ||
            !connected_future.get())
        {
            throw AppError::internal("could not attach to the PDF renderer");
        }
    }

    /// Sends a command and blocks until its reply arrives.
    json call(const std::string &method,
              json params = json::object(),
              const std::string &session_id = {},
              std::chrono::seconds timeout = kRenderTimeout)
    {
        int id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = next_id_++;
        }

        send(id, method, std::move(params), session_id);

        std::unique_lock<std::mutex> lock(mutex_);
        const bool arrived = condition_.wait_for(lock, timeout, [&] {
            return closed_ || responses_.count(id) > 0;
        });

        const auto it = responses_.find(id);
        if (!arrived || it == responses_.end())
        {
            throw AppError::internal("PDF renderer stopped responding");
        }

        json response = std::move(it->second);
        responses_.erase(it);
        lock.unlock();

        if (response.contains("error"))
        {
            throw AppError::internal(
                "PDF renderer rejected " + method + ": " +
                response["error"].value("message", "unknown error"));
        }
        return response.value("result", json::object());
    }

    /// Waits for a protocol event, e.g. Page.loadEventFired.
    void wait_for_event(const std::string &method,
                        std::chrono::seconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool arrived = condition_.wait_for(lock, timeout, [&] {
            return closed_ || seen_events_.count(method) > 0;
        });
        if (!arrived || seen_events_.count(method) == 0)
        {
            throw AppError::internal("the page never finished loading");
        }
    }

    /// Session id for the attached page target; set once attached so the
    /// request interceptor can answer events on its own.
    void set_session_id(std::string session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        page_session_id_ = std::move(session_id);
    }

  private:
    void send(int id,
              const std::string &method,
              json params,
              const std::string &session_id)
    {
        json message = {{"id", id}, {"method", method}, {"params", params}};
        if (!session_id.empty())
        {
            message["sessionId"] = session_id;
        }
        const std::string payload = message.dump();

        auto connection = client_->getConnection();
        if (!connection || !connection->connected())
        {
            throw AppError::internal("PDF renderer connection is gone");
        }
        connection->send(payload);
    }

    void on_message(const std::string &message)
    {
        auto parsed = json::parse(message, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            return;
        }

        if (parsed.contains("id") && parsed["id"].is_number_integer())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            responses_[parsed["id"].get<int>()] = std::move(parsed);
            condition_.notify_all();
            return;
        }

        const std::string method = parsed.value("method", "");
        if (method == "Fetch.requestPaused")
        {
            handle_paused_request(parsed);
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        seen_events_.insert(method);
        condition_.notify_all();
    }

    /// The SSRF lock. Only the temp file holding the export HTML and already
    /// inlined `data:` URIs are allowed through; everything else — http(s),
    /// ws, IP literals, anything Chromium would otherwise dial out to — is
    /// failed before a connection is opened.
    void handle_paused_request(const json &event)
    {
        const auto &params = event["params"];
        const std::string request_id = params.value("requestId", "");
        const std::string url =
            params.contains("request")
                ? params["request"].value("url", std::string{})
                : std::string{};
        const std::string session_id = event.value("sessionId", "");

        const bool allowed = util::starts_with(url, "file://") ||
                             util::starts_with(url, "data:");

        int id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = next_id_++;
        }

        try
        {
            if (allowed)
            {
                send(id,
                     "Fetch.continueRequest",
                     json{{"requestId", request_id}},
                     session_id);
            }
            else
            {
                spdlog::debug("PDF export blocked a request to {}", url);
                send(id,
                     "Fetch.failRequest",
                     json{{"requestId", request_id},
                          {"errorReason", "BlockedByClient"}},
                     session_id);
            }
        }
        catch (const std::exception &)
        {
            // The connection died mid-render; the waiting call() will time
            // out and report it.
        }
    }

    drogon::WebSocketClientPtr client_;
    std::mutex mutex_;
    std::condition_variable condition_;
    int next_id_ = 1;
    bool closed_ = false;
    std::string page_session_id_;
    std::map<int, json> responses_;
    std::set<std::string> seen_events_;
};

/// Chromium writes `<user-data-dir>/DevToolsActivePort` once it is listening:
/// port on the first line, browser WebSocket path on the second.
std::pair<uint16_t, std::string> wait_for_devtools_port(
    const fs::path &user_data_dir,
    std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const fs::path port_file = user_data_dir / "DevToolsActivePort";

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (const auto contents = util::read_file(port_file))
        {
            const auto lines = util::split(*contents, '\n');
            if (lines.size() >= 2)
            {
                try
                {
                    const auto port = static_cast<uint16_t>(
                        std::stoi(util::trim(lines[0])));
                    const std::string path = util::trim(lines[1]);
                    if (port != 0 && !path.empty())
                    {
                        return {port, path};
                    }
                }
                catch (const std::exception &)
                {
                    // Half-written file; try again on the next tick.
                }
            }
        }
        std::this_thread::sleep_for(50ms);
    }

    throw AppError::internal("the PDF renderer never became ready");
}

/// A temp directory removed when the render finishes, whatever the outcome.
class ScopedTempDir
{
  public:
    ScopedTempDir()
        : path_(fs::temp_directory_path() /
                ("lectern-export-" + util::random_hex(12)))
    {
        std::error_code ec;
        fs::create_directories(path_, ec);
        if (ec)
        {
            throw AppError::internal("cannot create a temp directory");
        }
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    const fs::path &path() const
    {
        return path_;
    }

  private:
    fs::path path_;
};

std::string render_pdf(const std::string &html)
{
    const ScopedTempDir temp;
    const fs::path html_file = temp.path() / "export.html";
    util::write_file_atomic(html_file, html);

    const fs::path profile_dir = temp.path() / "profile";
    std::error_code ec;
    fs::create_directories(profile_dir, ec);

    ChildProcess browser(
        find_browser(),
        {
            "--headless=new",
            "--disable-gpu",
            // Containers rarely allow the sandbox's namespaces; the render is
            // locked down by the CDP policy below instead.
            "--no-sandbox",
            // Docker gives a container 64MB of /dev/shm by default, which
            // Chromium exhausts on anything but a trivial page — it then dies
            // mid-render and the export fails with a timeout that points
            // nowhere near the real cause. This makes it use temp files
            // instead, which is the standard fix for running it in a
            // container and costs nothing elsewhere.
            "--disable-dev-shm-usage",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-extensions",
            "--disable-background-networking",
            "--disable-sync",
            "--mute-audio",
            // Belt to the Fetch interceptor's braces: even a request that
            // somehow escaped interception resolves to a dead address.
            "--host-resolver-rules=MAP * 0.0.0.0",
            "--remote-debugging-port=0",
            "--user-data-dir=" + profile_dir.string(),
        });

    const auto [port, browser_path] =
        wait_for_devtools_port(profile_dir, kRenderTimeout);

    CdpSession session;
    session.connect(port, browser_path, kRenderTimeout);

    const json target =
        session.call("Target.createTarget", {{"url", "about:blank"}});
    const std::string target_id = target.value("targetId", "");
    if (target_id.empty())
    {
        throw AppError::internal("the PDF renderer opened no page");
    }

    const json attached = session.call(
        "Target.attachToTarget", {{"targetId", target_id}, {"flatten", true}});
    const std::string session_id = attached.value("sessionId", "");
    if (session_id.empty())
    {
        throw AppError::internal("could not attach to the rendered page");
    }
    session.set_session_id(session_id);

    // The export HTML needs no scripting — mermaid diagrams and notebook
    // pages are pre-rendered to static SVG/PNG client-side — so turning it
    // off removes the entire script-execution surface.
    session.call("Emulation.setScriptExecutionDisabled",
                 {{"value", true}},
                 session_id);

    // Every request now pauses for the allowlist in handle_paused_request.
    session.call("Fetch.enable", json::object(), session_id);
    session.call("Page.enable", json::object(), session_id);

    const std::string file_url = "file:///" + [&] {
        std::string path = html_file.generic_string();
        // Windows paths start with a drive letter, POSIX with a slash that
        // would double up after the "file:///" prefix.
        if (!path.empty() && path.front() == '/')
        {
            path.erase(0, 1);
        }
        return path;
    }();

    session.call("Page.navigate", {{"url", file_url}}, session_id);
    session.wait_for_event("Page.loadEventFired", kRenderTimeout);

    const json printed = session.call("Page.printToPDF",
                                      {{"displayHeaderFooter", false},
                                       {"printBackground", true},
                                       {"preferCSSPageSize", true}},
                                      session_id);

    const auto decoded =
        util::base64_decode(printed.value("data", std::string{}));
    if (!decoded || decoded->empty())
    {
        throw AppError::internal("the PDF renderer produced nothing");
    }
    return *decoded;
}

}  // namespace

void init()
{
    cdp_loop().run();
}

void register_routes()
{
    drogon::app().registerHandler(
        "/api/export/pdf",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            // Identity is read here, on the event loop, before any work is
            // handed off — the request object must not outlive the handler
            // in a way the render thread depends on.
            std::string user_id;
            std::string html;
            try
            {
                user_id = auth::current_user_id(req);
                html = std::string(req->getBody());
            }
            catch (const AppError &error)
            {
                callback(http::error_response(error));
                return;
            }

            std::thread([callback = std::move(callback),
                         user_id = std::move(user_id),
                         html = std::move(html)]() mutable {
                http::guard(
                    std::move(callback), [&]() -> drogon::HttpResponsePtr {
                        // The renderer below only ever accepts file:// and
                        // data: requests, so any custom fonts the account
                        // uploaded have to already be inline as data: URIs
                        // before Chromium sees the page — an http:// url()
                        // back to this server would just be dropped.
                        const std::string font_style =
                            fonts::embed_account_fonts_style(user_id);

                        std::string document;
                        const size_t head = html.find("<head>");
                        if (head != std::string::npos)
                        {
                            const size_t insert_at =
                                head + std::string_view("<head>").size();
                            document.reserve(html.size() + font_style.size());
                            document.append(html, 0, insert_at);
                            document.append(font_style);
                            document.append(html, insert_at,
                                            std::string::npos);
                        }
                        else
                        {
                            document = font_style + html;
                        }

                        // Hold a render slot for the whole render, and give
                        // up rather than queue forever.
                        if (!render_slots().try_acquire_for(kQueueTimeout))
                        {
                            throw AppError::internal(
                                "PDF renderer is busy, try again");
                        }
                        struct SlotGuard
                        {
                            ~SlotGuard()
                            {
                                render_slots().release();
                            }
                        } slot_guard;

                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setContentTypeCodeAndCustomString(
                            drogon::CT_CUSTOM, "application/pdf");
                        resp->setBody(render_pdf(document));
                        return resp;
                    });
            }).detach();
        },
        {drogon::Post});
}

}  // namespace lectern::pdf_export
