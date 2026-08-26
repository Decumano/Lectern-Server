#pragma once

#include <drogon/HttpRequest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "db.h"

namespace lectern {

class Releases;

/// In-memory fixed-window counters keyed by (client IP, bucket name), used to
/// throttle auth abuse: password brute force on login and mass account
/// creation on register. Behind a reverse proxy every client shares the
/// proxy's IP, so login only counts *failed* attempts — something legitimate
/// users rarely rack up — rather than all traffic.
class RateLimiter
{
  public:
    using Clock = std::chrono::steady_clock;

    /// True when the bucket already reached `max` hits inside `window`.
    bool is_limited(const std::string &ip,
                    const std::string &kind,
                    uint32_t max,
                    std::chrono::seconds window);

    /// Records one hit against the bucket, starting a fresh window if the
    /// previous one expired.
    void record(const std::string &ip,
                const std::string &kind,
                std::chrono::seconds window);

  private:
    /// Longest window any caller uses; entries older than this are dropped on
    /// the next hit so the map can't grow without bound.
    static constexpr std::chrono::seconds kMaxWindow{60 * 60};

    struct Bucket
    {
        uint32_t count = 0;
        Clock::time_point start;
    };

    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Bucket> buckets_;
};

struct AppState
{
    std::shared_ptr<Db> db;
    std::filesystem::path workspaces_dir;
    std::filesystem::path fonts_dir;
    RateLimiter auth_limiter;
    std::shared_ptr<Releases> releases;

    /// Peer addresses (the reverse proxy in front of this server) whose
    /// X-Forwarded-For header is trusted for rate-limit bucketing. Parsed
    /// once from TRUSTED_PROXIES at startup.
    std::set<std::string> trusted_proxies;

    /// Per-account workspace size ceiling in bytes; 0 disables the check.
    uint64_t workspace_quota_bytes = 0;

    /// Signs session cookies. Empty means unsigned (local dev only).
    std::string session_secret;

    /// Whether session cookies carry the Secure flag.
    bool cookie_secure = false;

    /// The IP the rate limiter should bucket a request under. Directly
    /// exposed servers use the TCP peer address. Behind a reverse proxy every
    /// request arrives from the proxy's IP — one shared bucket, so a single
    /// abuser could exhaust `register`/`anon-comment` for everyone. When the
    /// peer is listed in TRUSTED_PROXIES, use the rightmost X-Forwarded-For
    /// entry instead: that's the value our own proxy appended, the entries
    /// left of it are client-controlled and stay untrusted.
    std::string client_ip(const drogon::HttpRequestPtr &req) const;
};

/// The process-wide state, set up once in main().
AppState &state();

}  // namespace lectern
