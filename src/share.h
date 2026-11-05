// Sharing and comments. An owner can share a file or folder from their
// workspace with another registered account at one of three permission
// levels (view < comment < edit); grantees reach the shared content through
// the /api/shared/* endpoints, which resolve a share id (never a raw owner
// path) and enforce the granted level server-side. Comments are stored
// against the file's owner + path so everyone with access sees one thread,
// and are only allowed on work files.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace lectern::share {

void fRegisterRoutes();

/// Resolves a sync-socket `watch` request ({share, path} for a grantee,
/// {link, path} for a link viewer) to the owner workspace and the path inside
/// it, or nullopt when this user may not follow it. Never throws.
std::optional<std::pair<std::string, std::string>> fResolveWatch(
    const std::string &user_id, const nlohmann::json &request);

}  // namespace lectern::share
