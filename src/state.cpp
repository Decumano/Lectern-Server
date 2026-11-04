#include "state.h"

#include "util.h"

namespace lectern {

bool RateLimiter::fIsLimited(const std::string &ip,
                             const std::string &kind,
                             uint32_t max,
                             std::chrono::seconds window)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = buckets_.find({ip, kind});
    if (it == buckets_.end())
    {
        return false;
    }
    if (Clock::now() - it->second.start >= window)
    {
        return false;
    }
    return it->second.count >= max;
}

void RateLimiter::fRecord(const std::string &ip,
                         const std::string &kind,
                         std::chrono::seconds window)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = Clock::now();
    for (auto it = buckets_.begin(); it != buckets_.end();)
    {
        it = (now - it->second.start >= cMaxWindow) ? buckets_.erase(it)
                                                    : std::next(it);
    }

    auto &bucket = buckets_[{ip, kind}];
    if (bucket.count == 0 || now - bucket.start >= window)
    {
        bucket.count = 0;
        bucket.start = now;
    }
    ++bucket.count;
}

std::string AppState::fClientIp(const drogon::HttpRequestPtr &req) const
{
    const std::string peer = req->peerAddr().toIp();
    if (trusted_proxies.find(peer) == trusted_proxies.end())
    {
        return peer;
    }

    const std::string forwarded = req->getHeader("x-forwarded-for");
    if (forwarded.empty())
    {
        return peer;
    }

    // Rightmost entry only: that's the one our own proxy appended. Everything
    // to its left is client-supplied and must stay untrusted.
    const size_t pos = forwarded.rfind(',');
    const std::string last = util::fTrim(
        pos == std::string::npos ? forwarded : forwarded.substr(pos + 1));
    return last.empty() ? peer : last;
}

AppState &fState()
{
    static AppState instance;
    return instance;
}

}  // namespace lectern
