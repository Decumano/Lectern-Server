// Workspace file CRUD, ported from the Tauri backend in the original Lectern
// desktop app. There, `root` came straight from the client because it was
// gated by an OS folder-picker dialog for a single local user. On the web
// there is no such gate, so `root` is instead always derived from the
// authenticated session, and every relative path is validated with
// `safe_rel_path` to make sure it can't escape that root.
#pragma once

#include <drogon/HttpRequest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lectern::workspace {

inline constexpr std::array<std::string_view, 8> kWorkFileExtensions = {
    "mdp", "mds", "mdg", "mdn", "mdl", "mdc", "mde", "mdb"};

/// Root-level sidecar files that sync clients need to see in listings (with a
/// hash) even though they aren't work files: the user's custom document
/// templates and roaming UI preferences (theme), so both follow the account
/// across devices. The web UI filters these out of its file tree; the desktop
/// tree never lists them (its walker only shows work extensions).
inline constexpr std::array<std::string_view, 2> kSyncSidecarFiles = {
    "_lktpl.json", "_lkprefs.json"};

/// Rejects absolute paths, drive-letter prefixes, and `..` components so a
/// request can never resolve to a path outside the caller's workspace root.
/// Returns the path in canonical rel form: forward slashes, no redundant
/// separators — the same string clients use in every request and listing.
/// Throws AppError::bad_request on anything suspicious.
std::string safe_rel_path(std::string_view rel_path);

bool is_work_file(std::string_view rel_path);

/// The listing shape the frontend and sync clients consume: name, relPath,
/// isDir, modified, hash, children.
nlohmann::json walk_work_dir(const std::filesystem::path &dir,
                             const std::string &rel_prefix);

/// Cheap change-detection for one file: lets a client poll "did anyone else
/// write this?" without downloading the content or walking the whole tree.
nlohmann::json stat_file(const std::filesystem::path &path);

/// Registration is open, so an unbounded workspace lets any account fill the
/// server's disk. Checked before a write that would grow the workspace;
/// replacing a file with a smaller one always succeeds, so a user who hits
/// the ceiling can still edit their way back under it.
void check_quota(const std::filesystem::path &root,
                 const std::filesystem::path &target,
                 uint64_t incoming_len);

/// The caller's workspace root, created if missing.
std::filesystem::path user_root(const drogon::HttpRequestPtr &req,
                                std::string *user_id_out = nullptr);

void register_routes();

}  // namespace lectern::workspace
