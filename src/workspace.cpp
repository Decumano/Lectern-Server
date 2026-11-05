#include "workspace.h"

#include <drogon/HttpAppFramework.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "auth.h"
#include "error.h"
#include "http.h"
#include "state.h"
#include "sync_ws.h"
#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::workspace {

namespace {

/// Largest single work file accepted. The Rust build originally inherited
/// axum's 2MB default here, which a .mdn notebook — freehand ink, imported
/// PDF pages, images — passes routinely, so the first sync of such a
/// workspace failed with 413. WORKSPACE_QUOTA_BYTES still caps the account.
size_t fMaxWorkFileBytes()
{
    static const size_t value = static_cast<size_t>(
        util::fEnvI64("MAX_WORK_FILE_BYTES", 32LL * 1024 * 1024));
    return value;
}

uint64_t fModifiedMs(const fs::path &path)
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

template <size_t N>
bool fContains(const std::array<std::string_view, N> &haystack,
              std::string_view needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) !=
           haystack.end();
}

/// Total bytes stored under a workspace root. Walks the tree rather than
/// tracking a running total in the DB: workspaces are small (text files) and
/// this way the number can't drift out of sync with what's on disk.
uint64_t fDirSizeBytes(const fs::path &dir)
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

void fRecordTombstone(const std::string &user_id, const std::string &rel_path)
{
    try
    {
        fState().db->fExecute(
            "INSERT INTO deleted_files (user_id, rel_path, deleted_at)"
            " VALUES (?, ?, ?)"
            " ON CONFLICT(user_id, rel_path)"
            " DO UPDATE SET deleted_at = excluded.deleted_at",
            {DbValue(user_id), DbValue(rel_path), DbValue(util::fNowMs())});
    }
    catch (const std::exception &)
    {
        // Best-effort, exactly as the Rust original: a failed tombstone must
        // not turn a successful delete into an error response.
    }
}

void fClearTombstone(const std::string &user_id, const std::string &rel_path)
{
    try
    {
        fState().db->fExecute(
            "DELETE FROM deleted_files WHERE user_id = ? AND rel_path = ?",
            {DbValue(user_id), DbValue(rel_path)});
    }
    catch (const std::exception &)
    {
    }
}

/// Every file (not directory) under `dir`, as rel keys prefixed with
/// `rel_prefix` — used to tombstone the contents of a deleted/moved folder.
void fCollectFilesUnder(const fs::path &dir,
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
            fCollectFilesUnder(entry.path(), rel, out);
        }
        else
        {
            out.push_back(rel);
        }
    }
}

}  // namespace

std::string fSafeRelPath(std::string_view rel_path)
{
    const auto fInvalid = [] {
        return AppError::fBadRequest("invalid path");
    };

    if (rel_path.empty())
    {
        throw fInvalid();
    }
    // A leading separator is an absolute path; Rust's Component::RootDir.
    if (rel_path.front() == '/' || rel_path.front() == '\\')
    {
        throw fInvalid();
    }

    std::vector<std::string> segments;
    std::string current;
    const auto fFlush = [&] {
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
            throw fInvalid();  // Component::ParentDir is refused outright
        }
        // A colon is either a Windows drive prefix ("C:") or an NTFS
        // alternate data stream ("file.mdp:evil"); neither belongs in a
        // workspace-relative path.
        if (current.find(':') != std::string::npos)
        {
            throw fInvalid();
        }
        segments.push_back(current);
        current.clear();
    };

    for (const char c : rel_path)
    {
        if (c == '/' || c == '\\')
        {
            fFlush();
        }
        else if (c == '\0')
        {
            throw fInvalid();
        }
        else
        {
            current.push_back(c);
        }
    }
    fFlush();

    if (segments.empty())
    {
        throw fInvalid();
    }

    std::string out = segments.front();
    for (size_t i = 1; i < segments.size(); ++i)
    {
        out += "/";
        out += segments[i];
    }
    return out;
}

bool fIsWorkFile(std::string_view rel_path)
{
    return fContains(cWorkFileExtensions, util::fExtensionOf(rel_path));
}

json fWalkWorkDir(const fs::path &dir, const std::string &rel_prefix)
{
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
    {
        throw AppError::fNotFound();
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
                                     fWalkWorkDir(entry.path(), rel_path)}}});
            continue;
        }

        // Every regular file is listed, not only the app formats. This
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
                  {"modified", fModifiedMs(entry.path())},
                  // sha256 hex digest of the file's bytes, empty for
                  // directories. Lets sync clients tell "did this change on
                  // the server" from one listing call without downloading
                  // every file's content.
                  {"hash", util::fSha256File(entry.path())},
                  {"children", json::array()}}});
    }

    std::sort(entries.begin(),
              entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.is_dir != b.is_dir)
                  {
                      return a.is_dir;  // directories first
                  }
                  return util::fToLower(a.name) < util::fToLower(b.name);
              });

    json out = json::array();
    for (auto &entry : entries)
    {
        out.push_back(std::move(entry.value));
    }
    return out;
}

json fStatFile(const fs::path &path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
    {
        throw AppError::fNotFound();
    }
    return json{{"modified", fModifiedMs(path)},
                {"hash", util::fSha256File(path)}};
}

void fCheckQuota(const fs::path &root,
                 const fs::path &target,
                 uint64_t incoming_len)
{
    const uint64_t quota = fState().workspace_quota_bytes;
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
        fDirSizeBytes(root) - existing_len + incoming_len;
    if (projected > quota)
    {
        throw AppError::fBadRequest(
            "workspace is full (" + std::to_string(quota / (1024 * 1024)) +
            " MB max) — delete something first");
    }
}

fs::path fUserRoot(const drogon::HttpRequestPtr &req, std::string *user_id_out)
{
    const std::string user_id = auth::fCurrentUserIdWithHeaders(req);
    if (user_id_out != nullptr)
    {
        *user_id_out = user_id;
    }
    const fs::path root = fState().workspaces_dir / user_id;
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

void fRegisterRoutes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/workspace",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                return http::fJsonResponse(
                    fWalkWorkDir(fUserRoot(req), ""));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/workspace/stat",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                const fs::path root = fUserRoot(req);
                const std::string rel =
                    fSafeRelPath(http::fRequiredQuery(req, "path"));
                return http::fJsonResponse(fStatFile(root / rel));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/workspace/file",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                std::string user_id;
                const fs::path root = fUserRoot(req, &user_id);
                const std::string rel =
                    fSafeRelPath(http::fRequiredQuery(req, "path"));
                const fs::path path = root / rel;

                if (req->method() == drogon::Get)
                {
                    const auto content = util::fReadFile(path);
                    if (!content)
                    {
                        throw AppError::fNotFound();
                    }
                    return http::fTextResponse(*content);
                }

                // A view, not a copy: Drogon already holds the whole
                // body in memory, and copying it doubled the peak cost
                // of every upload -- which matters once
                // MAX_WORK_FILE_BYTES is raised for large attachments.
                const std::string_view content = req->getBody();
                // Drogon's body limit is global and has to accommodate PDF
                // export, so the per-file ceiling is enforced here.
                if (content.size() > fMaxWorkFileBytes())
                {
                    throw AppError::fBadRequest(
                        "file is too large (" +
                        std::to_string(fMaxWorkFileBytes() / (1024 * 1024)) +
                        " MB max)");
                }
                fCheckQuota(root, path, content.size());
                util::fWriteFileAtomic(path, content);

                // Writing a path makes it a live file again; a stale
                // tombstone would tell sync clients to delete what the user
                // just (re)created.
                fClearTombstone(user_id, rel);
                sync::fNotifyChanges(user_id, {rel}, req->getHeader("x-lectern-device"));
                return http::fOkResponse();
            });
        },
        {drogon::Get, drogon::Put});

    app.registerHandler(
        "/api/workspace/folder",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                std::string user_id;
                const fs::path root = fUserRoot(req, &user_id);
                const std::string rel = fSafeRelPath(
                    http::fJsonString(http::fJsonBody(req), "relPath"));
                std::error_code ec;
                fs::create_directories(root / rel, ec);
                if (ec)
                {
                    throw AppError::fInternal(ec.message());
                }
                // A folder is not a file: tell the owner's devices to look at everything.
                sync::fNotifyChanges(user_id, {}, req->getHeader("x-lectern-device"));
                return http::fOkResponse();
            });
        },
        {drogon::Post});

    app.registerHandler(
        "/api/workspace/entry",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                std::string user_id;
                const fs::path root = fUserRoot(req, &user_id);
                const std::string rel =
                    fSafeRelPath(http::fRequiredQuery(req, "path"));
                const fs::path path = root / rel;
                const bool is_dir = http::fQueryBool(req, "isDir");

                std::error_code ec;
                if (is_dir)
                {
                    std::vector<std::string> inside;
                    fCollectFilesUnder(path, rel, inside);
                    if (fs::remove_all(path, ec) == 0 || ec)
                    {
                        throw AppError::fNotFound();
                    }
                    for (const auto &file_rel : inside)
                    {
                        fRecordTombstone(user_id, file_rel);
                    }
                }
                else
                {
                    if (!fs::remove(path, ec) || ec)
                    {
                        throw AppError::fNotFound();
                    }
                    fRecordTombstone(user_id, rel);
                }
                sync::fNotifyChanges(user_id,
                                     is_dir ? std::vector<std::string>{} : std::vector<std::string>{rel},
                                     req->getHeader("x-lectern-device"));
                return http::fOkResponse();
            });
        },
        {drogon::Delete});

    app.registerHandler(
        "/api/workspace/move",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                std::string user_id;
                const fs::path root = fUserRoot(req, &user_id);
                const auto body = http::fJsonBody(req);
                const std::string from_key =
                    fSafeRelPath(http::fJsonString(body, "from"));
                const std::string to_key =
                    fSafeRelPath(http::fJsonString(body, "to"));
                const fs::path from = root / from_key;
                const fs::path to = root / to_key;

                // A move is a delete at `from` plus a create at `to`,
                // tombstone-wise: stale copies of the old path must not
                // resurface, and any old tombstone on the destination must
                // not kill the file that now lives there.
                std::vector<std::string> moved_files;
                std::error_code ec;
                bool was_dir = false;
                if (fs::is_directory(from, ec) && !ec)
                {
                    was_dir = true;
                    fCollectFilesUnder(from, from_key, moved_files);
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
                    throw AppError::fNotFound();
                }

                for (const auto &file_rel : moved_files)
                {
                    fRecordTombstone(user_id, file_rel);
                    const std::string dest =
                        to_key + file_rel.substr(from_key.size());
                    fClearTombstone(user_id, dest);
                }
                sync::fNotifyChanges(user_id,
                                     was_dir ? std::vector<std::string>{}
                                             : std::vector<std::string>{from_key, to_key},
                                     req->getHeader("x-lectern-device"));
                return http::fOkResponse();
            });
        },
        {drogon::Post});

    // Tombstone listing for sync clients: files this workspace deleted, with
    // when. Lets a client holding an unknown local copy distinguish "deleted
    // elsewhere, drop it" from "created locally, push it".
    app.registerHandler(
        "/api/workspace/deleted",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                std::string user_id;
                fUserRoot(req, &user_id);
                const auto rows = fState().db->fQueryAll(
                    "SELECT rel_path, deleted_at FROM deleted_files"
                    " WHERE user_id = ?",
                    {DbValue(user_id)});

                json out = json::array();
                for (const auto &row : rows)
                {
                    out.push_back({{"relPath", row.fText(0)},
                                   {"deletedAt", row.fI64(1)}});
                }
                return http::fJsonResponse(out);
            });
        },
        {drogon::Get});
}

}  // namespace lectern::workspace
