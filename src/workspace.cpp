#include "workspace.h"

#include <drogon/HttpAppFramework.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "auth.h"
#include "error.h"
#include "http.h"
#include "state.h"
#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::workspace {

namespace {

/// Largest single work file accepted. The Rust build originally inherited
/// axum's 2MB default here, which a .mdn notebook — freehand ink, imported
/// PDF pages, images — passes routinely, so the first sync of such a
/// workspace failed with 413. WORKSPACE_QUOTA_BYTES still caps the account.
size_t max_work_file_bytes()
{
    static const size_t value = static_cast<size_t>(
        util::env_i64("MAX_WORK_FILE_BYTES", 32LL * 1024 * 1024));
    return value;
}

uint64_t modified_ms(const fs::path &path)
{
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
    {
        return 0;
    }
    // file_time_type's epoch is unspecified. std::chrono::clock_cast is the
    // clean way across, but libstdc++ does not implement it for file_clock
    // -- GCC 12 on bookworm, which is what the container builds with -- so
    // shift by the offset between the two clocks instead. The two now()
    // calls land microseconds apart and this returns whole milliseconds.
    const auto system_time =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - fs::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    const auto since_epoch = system_time.time_since_epoch();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch)
            .count();
    return millis < 0 ? 0 : static_cast<uint64_t>(millis);
}

bool contains(const std::array<std::string_view, 8> &haystack,
              std::string_view needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) !=
           haystack.end();
}

/// Total bytes stored under a workspace root. Walks the tree rather than
/// tracking a running total in the DB: workspaces are small (text files) and
/// this way the number can't drift out of sync with what's on disk.
uint64_t dir_size_bytes(const fs::path &dir)
{
    std::error_code ec;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        std::error_code stat_ec;
        if (it->is_regular_file(stat_ec) && !stat_ec)
        {
            total += fs::file_size(it->path(), stat_ec);
        }
    }
    return total;
}

// ── Deletion tombstones (see migrations/0007_deleted_files.sql) ──

void record_tombstone(const std::string &user_id, const std::string &rel_path)
{
    try
    {
        state().db->execute(
            "INSERT INTO deleted_files (user_id, rel_path, deleted_at)"
            " VALUES (?, ?, ?)"
            " ON CONFLICT(user_id, rel_path)"
            " DO UPDATE SET deleted_at = excluded.deleted_at",
            {DbValue(user_id), DbValue(rel_path), DbValue(util::now_ms())});
    }
    catch (const std::exception &)
    {
        // Best-effort, exactly as the Rust original: a failed tombstone must
        // not turn a successful delete into an error response.
    }
}

void clear_tombstone(const std::string &user_id, const std::string &rel_path)
{
    try
    {
        state().db->execute(
            "DELETE FROM deleted_files WHERE user_id = ? AND rel_path = ?",
            {DbValue(user_id), DbValue(rel_path)});
    }
    catch (const std::exception &)
    {
    }
}

/// Every file (not directory) under `dir`, as rel keys prefixed with
/// `rel_prefix` — used to tombstone the contents of a deleted/moved folder.
void collect_files_under(const fs::path &dir,
                         const std::string &rel_prefix,
                         std::vector<std::string> &out)
{
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec))
    {
        if (ec)
        {
            return;
        }
        const std::string name = entry.path().filename().string();
        const std::string rel =
            rel_prefix.empty() ? name : rel_prefix + "/" + name;

        std::error_code kind_ec;
        if (entry.is_directory(kind_ec) && !kind_ec)
        {
            collect_files_under(entry.path(), rel, out);
        }
        else
        {
            out.push_back(rel);
        }
    }
}

}  // namespace

std::string safe_rel_path(std::string_view rel_path)
{
    const auto invalid = [] {
        return AppError::bad_request("invalid path");
    };

    if (rel_path.empty())
    {
        throw invalid();
    }
    // A leading separator is an absolute path; Rust's Component::RootDir.
    if (rel_path.front() == '/' || rel_path.front() == '\\')
    {
        throw invalid();
    }

    std::vector<std::string> segments;
    std::string current;
    const auto flush = [&] {
        if (current.empty())
        {
            return;  // "a//b" collapses, same as Path::components()
        }
        if (current == ".")
        {
            current.clear();
            return;  // Component::CurDir is skipped
        }
        if (current == "..")
        {
            throw invalid();  // Component::ParentDir is refused outright
        }
        // A colon is either a Windows drive prefix ("C:") or an NTFS
        // alternate data stream ("file.mdp:evil"); neither belongs in a
        // workspace-relative path.
        if (current.find(':') != std::string::npos)
        {
            throw invalid();
        }
        segments.push_back(current);
        current.clear();
    };

    for (const char c : rel_path)
    {
        if (c == '/' || c == '\\')
        {
            flush();
        }
        else if (c == '\0')
        {
            throw invalid();
        }
        else
        {
            current.push_back(c);
        }
    }
    flush();

    if (segments.empty())
    {
        throw invalid();
    }

    std::string out = segments.front();
    for (size_t i = 1; i < segments.size(); ++i)
    {
        out += "/";
        out += segments[i];
    }
    return out;
}

bool is_work_file(std::string_view rel_path)
{
    return contains(kWorkFileExtensions, util::extension_of(rel_path));
}

json walk_work_dir(const fs::path &dir, const std::string &rel_prefix)
{
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        throw AppError::not_found();
    }

    struct Entry
    {
        std::string name;
        bool is_dir = false;
        json value;
    };
    std::vector<Entry> entries;

    for (const auto &entry : it)
    {
        const std::string name = entry.path().filename().string();
        const std::string rel_path =
            rel_prefix.empty() ? name : rel_prefix + "/" + name;

        std::error_code kind_ec;
        const bool is_dir = entry.is_directory(kind_ec);
        if (kind_ec)
        {
            continue;
        }

        if (is_dir)
        {
            entries.push_back({name,
                               true,
                               json{{"name", name},
                                    {"relPath", rel_path},
                                    {"isDir", true},
                                    {"modified", 0},
                                    {"hash", ""},
                                    {"children",
                                     walk_work_dir(entry.path(), rel_path)}}});
            continue;
        }

        // Every regular file is listed, not only the eight app formats. This
        // listing is also what a sync client reads to learn remote state, so
        // anything hidden here but present on disk looks deleted on the next
        // pass -- and the client would dutifully remove its local copy.
        // Dotfiles stay out: they are sync metadata, never user content.
        if (!name.empty() && name.front() == '.')
        {
            continue;
        }

        entries.push_back(
            {name,
             false,
             json{{"name", name},
                  {"relPath", rel_path},
                  {"isDir", false},
                  {"modified", modified_ms(entry.path())},
                  // sha256 hex digest of the file's bytes, empty for
                  // directories. Lets sync clients tell "did this change on
                  // the server" from one listing call without downloading
                  // every file's content.
                  {"hash", util::sha256_file(entry.path())},
                  {"children", json::array()}}});
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.is_dir != b.is_dir)
                  {
                      return a.is_dir;  // directories first
                  }
                  return util::to_lower(a.name) < util::to_lower(b.name);
              });

    json out = json::array();
    for (auto &entry : entries)
    {
        out.push_back(std::move(entry.value));
    }
    return out;
}

json stat_file(const fs::path &path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
    {
        throw AppError::not_found();
    }
    return json{{"modified", modified_ms(path)},
                {"hash", util::sha256_file(path)}};
}

void check_quota(const fs::path &root,
                 const fs::path &target,
                 uint64_t incoming_len)
{
    const uint64_t quota = state().workspace_quota_bytes;
    if (quota == 0)
    {
        return;
    }

    std::error_code ec;
    const uint64_t existing_len =
        fs::is_regular_file(target, ec) ? fs::file_size(target, ec) : 0;
    if (ec || incoming_len <= existing_len)
    {
        return;
    }

    const uint64_t projected =
        dir_size_bytes(root) - existing_len + incoming_len;
    if (projected > quota)
    {
        throw AppError::bad_request(
            "workspace is full (" + std::to_string(quota / (1024 * 1024)) +
            " MB max) — delete something first");
    }
}

fs::path user_root(const drogon::HttpRequestPtr &req, std::string *user_id_out)
{
    const std::string user_id = auth::current_user_id_with_headers(req);
    if (user_id_out != nullptr)
    {
        *user_id_out = user_id;
    }
    const fs::path root = state().workspaces_dir / user_id;
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

void register_routes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/workspace",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                return http::json_response(
                    walk_work_dir(user_root(req), ""));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/workspace/stat",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                const fs::path root = user_root(req);
                const std::string rel =
                    safe_rel_path(http::required_query(req, "path"));
                return http::json_response(stat_file(root / rel));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/workspace/file",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                std::string user_id;
                const fs::path root = user_root(req, &user_id);
                const std::string rel =
                    safe_rel_path(http::required_query(req, "path"));
                const fs::path path = root / rel;

                if (req->method() == drogon::Get)
                {
                    const auto content = util::read_file(path);
                    if (!content)
                    {
                        throw AppError::not_found();
                    }
                    return http::text_response(*content);
                }

                // A view, not a copy: Drogon already holds the whole
                // body in memory, and copying it doubled the peak cost
                // of every upload -- which matters once
                // MAX_WORK_FILE_BYTES is raised for large attachments.
                const std::string_view content = req->getBody();
                // Drogon's body limit is global and has to accommodate PDF
                // export, so the per-file ceiling is enforced here.
                if (content.size() > max_work_file_bytes())
                {
                    throw AppError::bad_request(
                        "file is too large (" +
                        std::to_string(max_work_file_bytes() / (1024 * 1024)) +
                        " MB max)");
                }
                check_quota(root, path, content.size());
                util::write_file_atomic(path, content);

                // Writing a path makes it a live file again; a stale
                // tombstone would tell sync clients to delete what the user
                // just (re)created.
                clear_tombstone(user_id, rel);
                return http::ok_response();
            });
        },
        {drogon::Get, drogon::Put});

    app.registerHandler(
        "/api/workspace/folder",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                const fs::path root = user_root(req);
                const std::string rel = safe_rel_path(
                    http::json_string(http::json_body(req), "relPath"));
                std::error_code ec;
                fs::create_directories(root / rel, ec);
                if (ec)
                {
                    throw AppError::internal(ec.message());
                }
                return http::ok_response();
            });
        },
        {drogon::Post});

    app.registerHandler(
        "/api/workspace/entry",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                std::string user_id;
                const fs::path root = user_root(req, &user_id);
                const std::string rel =
                    safe_rel_path(http::required_query(req, "path"));
                const fs::path path = root / rel;
                const bool is_dir = http::query_bool(req, "isDir");

                std::error_code ec;
                if (is_dir)
                {
                    std::vector<std::string> inside;
                    collect_files_under(path, rel, inside);
                    if (fs::remove_all(path, ec) == 0 || ec)
                    {
                        throw AppError::not_found();
                    }
                    for (const auto &file_rel : inside)
                    {
                        record_tombstone(user_id, file_rel);
                    }
                }
                else
                {
                    if (!fs::remove(path, ec) || ec)
                    {
                        throw AppError::not_found();
                    }
                    record_tombstone(user_id, rel);
                }
                return http::ok_response();
            });
        },
        {drogon::Delete});

    app.registerHandler(
        "/api/workspace/move",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                std::string user_id;
                const fs::path root = user_root(req, &user_id);
                const auto body = http::json_body(req);
                const std::string from_key =
                    safe_rel_path(http::json_string(body, "from"));
                const std::string to_key =
                    safe_rel_path(http::json_string(body, "to"));
                const fs::path from = root / from_key;
                const fs::path to = root / to_key;

                // A move is a delete at `from` plus a create at `to`,
                // tombstone-wise: stale copies of the old path must not
                // resurface, and any old tombstone on the destination must
                // not kill the file that now lives there.
                std::vector<std::string> moved_files;
                std::error_code ec;
                if (fs::is_directory(from, ec) && !ec)
                {
                    collect_files_under(from, from_key, moved_files);
                }
                else
                {
                    moved_files.push_back(from_key);
                }

                const fs::path parent = to.parent_path();
                if (!parent.empty())
                {
                    fs::create_directories(parent, ec);
                }

                fs::rename(from, to, ec);
                if (ec)
                {
                    throw AppError::not_found();
                }

                for (const auto &file_rel : moved_files)
                {
                    record_tombstone(user_id, file_rel);
                    const std::string dest =
                        to_key + file_rel.substr(from_key.size());
                    clear_tombstone(user_id, dest);
                }
                return http::ok_response();
            });
        },
        {drogon::Post});

    // Tombstone listing for sync clients: files this workspace deleted, with
    // when. Lets a client holding an unknown local copy distinguish "deleted
    // elsewhere, drop it" from "created locally, push it".
    app.registerHandler(
        "/api/workspace/deleted",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                std::string user_id;
                user_root(req, &user_id);
                const auto rows = state().db->query_all(
                    "SELECT rel_path, deleted_at FROM deleted_files"
                    " WHERE user_id = ?",
                    {DbValue(user_id)});

                json out = json::array();
                for (const auto &row : rows)
                {
                    out.push_back({{"relPath", row.text(0)},
                                   {"deletedAt", row.i64(1)}});
                }
                return http::json_response(out);
            });
        },
        {drogon::Get});
}

}  // namespace lectern::workspace
