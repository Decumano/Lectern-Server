// Server-side bridge to the GitHub Releases API for the Download page.
// The desktop-app repo can stay private: this server holds the GitHub token
// (GITHUB_RELEASES_TOKEN, never sent to browsers), lists the latest release,
// and passes the installer assets through to visitors. Only the one repo
// configured at startup is reachable, and assets are addressed by numeric id,
// so this is not an open proxy. With a public repo the token is unnecessary —
// the bridge then just saves visitors from GitHub's anonymous rate limits.
#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace lectern {

class Releases
{
  public:
    static std::shared_ptr<Releases> from_env();

    const std::string &repo() const
    {
        return repo_;
    }

    const std::optional<std::string> &token() const
    {
        return token_;
    }

    /// The cached listing when it is still fresh.
    std::optional<nlohmann::json> cached();
    void store(nlohmann::json view);

  private:
    /// How long a fetched release listing is served from memory before asking
    /// GitHub again. Keeps a busy download page to ~12 API calls per hour.
    static constexpr std::chrono::seconds kCacheTtl{300};

    std::string repo_;
    std::optional<std::string> token_;

    std::mutex mutex_;
    std::optional<nlohmann::json> cache_;
    std::chrono::steady_clock::time_point cached_at_;
};

namespace downloads {

void register_routes();

}  // namespace downloads
}  // namespace lectern
