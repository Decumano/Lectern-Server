#include "share.h"

#include <drogon/HttpAppFramework.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "auth.h"
#include "error.h"
#include "http.h"
#include "state.h"
#include "util.h"
#include "workspace.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace lectern::share {

namespace {

constexpr size_t kMaxCommentLen = 4000;
/// Anchors are small client-defined JSON blobs (text range / cell range /
/// item id); the server only bounds their size.
constexpr size_t kMaxAnchorLen = 2000;

// Anonymous link visitors can post comments; cap the rate per IP so a leaked
// comment-link can't be used to flood a document.
constexpr uint32_t kCommentLimit = 20;
constexpr std::chrono::seconds kCommentWindow = 10min;

/// Ordered so `perm < Perm::Edit` reads the same as it did in Rust.
enum class Perm
{
    View = 0,
    Comment = 1,
    Edit = 2,
};

std::optional<Perm> parse_perm(const std::string &value)
{
    if (value == "view")
    {
        return Perm::View;
    }
    if (value == "comment")
    {
        return Perm::Comment;
    }
    if (value == "edit")
    {
        return Perm::Edit;
    }
    return std::nullopt;
}

const char *perm_str(Perm perm)
{
    switch (perm)
    {
        case Perm::View:
            return "view";
        case Perm::Comment:
            return "comment";
        case Perm::Edit:
            return "edit";
    }
    return "view";
}

Perm perm_from_db(const std::string &value)
{
    const auto parsed = parse_perm(value);
    if (!parsed)
    {
        throw AppError::internal("bad permission in db");
    }
    return *parsed;
}

struct ShareRow
{
    std::string owner_id;
    std::string grantee_id;
    std::string rel_path;
    bool is_dir = false;
    std::string permission;
};

struct LinkRow
{
    std::string owner_id;
    std::string rel_path;
    bool is_dir = false;
    std::string permission;
};

/// Resolved access: whose workspace, which path inside it, at what level.
struct Access
{
    std::string owner_id;
    std::string rel_path;
    bool is_dir = false;
    Perm permission = Perm::View;
};

ShareRow load_share(const std::string &share_id)
{
    const auto row = state().db->query_one(
        "SELECT owner_id, grantee_id, rel_path, is_dir, permission"
        " FROM shares WHERE id = ?",
        {DbValue(share_id)});
    if (!row)
    {
        throw AppError::not_found();
    }
    return {row->text(0),
            row->text(1),
            row->text(2),
            row->boolean(3),
            row->text(4)};
}

LinkRow load_link(const std::string &token)
{
    const auto row = state().db->query_one(
        "SELECT owner_id, rel_path, is_dir, permission FROM share_links"
        " WHERE id = ?",
        {DbValue(token)});
    if (!row)
    {
        throw AppError::not_found();
    }
    return {row->text(0), row->text(1), row->boolean(2), row->text(3)};
}

/// Joins the optional sub path (folder shares only) safely under the shared
/// root, giving the owner-workspace-relative path.
std::string effective_path(const std::string &shared_root,
                           bool is_dir,
                           const std::string &sub_path,
                           const char *not_a_folder_message)
{
    if (sub_path.empty())
    {
        return shared_root;
    }
    if (!is_dir)
    {
        throw AppError::bad_request(not_a_folder_message);
    }
    return shared_root + "/" + workspace::safe_rel_path(sub_path);
}

/// Resolves a grantee's access through a share: checks the share belongs to
/// this user, then joins any sub path under the shared root.
Access resolve_share_access(const std::string &user_id,
                            const std::string &share_id,
                            const std::string &sub_path)
{
    const ShareRow share = load_share(share_id);
    if (share.grantee_id != user_id)
    {
        throw AppError::not_found();  // don't reveal that the share exists
    }
    return {share.owner_id,
            effective_path(share.rel_path,
                           share.is_dir,
                           sub_path,
                           "not a folder share"),
            share.is_dir,
            perm_from_db(share.permission)};
}

/// Same shape as `resolve_share_access`, but the token itself is the
/// authorization — no session or account involved.
Access resolve_link_access(const std::string &token,
                           const std::string &sub_path)
{
    const LinkRow link = load_link(token);
    return {link.owner_id,
            effective_path(link.rel_path,
                           link.is_dir,
                           sub_path,
                           "not a folder link"),
            link.is_dir,
            perm_from_db(link.permission)};
}

fs::path owner_full_path(const std::string &owner_id, const std::string &rel)
{
    // rel comes from the shares table (owner-created) plus a safe_rel_path'd
    // sub path, but re-validate the whole thing anyway before touching disk.
    return state().workspaces_dir / owner_id / workspace::safe_rel_path(rel);
}

/// Shared read of a work file through either a share or a link.
std::string read_through(const Access &access)
{
    if (!workspace::is_work_file(access.rel_path))
    {
        throw AppError::bad_request("not a work file");
    }
    const auto content =
        util::read_file(owner_full_path(access.owner_id, access.rel_path));
    if (!content)
    {
        throw AppError::not_found();
    }
    return *content;
}

/// Shared write of a work file through either a share or a link. Grantees can
/// edit existing files but not create new ones in the owner's workspace;
/// keeps an edit share from being used to fill someone's disk. Writes land in
/// the owner's workspace, so they count against the owner's quota — otherwise
/// an edit share is a way around it.
void write_through(const Access &access, const std::string &content)
{
    if (access.permission < Perm::Edit)
    {
        throw AppError::forbidden();
    }
    if (!workspace::is_work_file(access.rel_path))
    {
        throw AppError::bad_request("not a work file");
    }

    const fs::path full = owner_full_path(access.owner_id, access.rel_path);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec) || ec)
    {
        throw AppError::not_found();
    }

    const fs::path owner_root = state().workspaces_dir / access.owner_id;
    workspace::check_quota(owner_root, full, content.size());
    util::write_file_atomic(full, content);
}

/// Confirms the entry the owner is about to share actually exists.
void require_entry_exists(const std::string &owner_id,
                          const std::string &rel,
                          bool is_dir)
{
    const fs::path full = state().workspaces_dir / owner_id / rel;
    std::error_code ec;
    if (is_dir && (!fs::is_directory(full, ec) || ec))
    {
        throw AppError::bad_request("no such folder");
    }
    if (!is_dir && (!fs::is_regular_file(full, ec) || ec))
    {
        throw AppError::bad_request("no such file");
    }
}

// ── Comment targets ──

/// The three mutually exclusive ways a comments request names its file.
struct CommentTarget
{
    std::string path;      ///< owner context: path of my own file
    std::string share;     ///< grantee context: share id
    std::string link;      ///< link context: token, works without a login
    std::string sub_path;  ///< optional sub path for folder shares/links
};

CommentTarget target_from_query(const drogon::HttpRequestPtr &req)
{
    return {http::query(req, "path"),
            http::query(req, "share"),
            http::query(req, "link"),
            http::query(req, "subPath")};
}

CommentTarget target_from_body(const json &body)
{
    return {http::json_string_or(body, "path"),
            http::json_string_or(body, "share"),
            http::json_string_or(body, "link"),
            http::json_string_or(body, "subPath")};
}

/// Works out which file a comments request is about and whether the caller
/// may see / post at the required level. `user_id` is nullopt for anonymous
/// link visitors, which is only acceptable in the link context.
std::pair<std::string, std::string> resolve_comment_target(
    const std::optional<std::string> &user_id,
    const CommentTarget &target,
    Perm need)
{
    if (!target.link.empty())
    {
        const Access access =
            resolve_link_access(target.link, target.sub_path);
        if (access.permission < need)
        {
            throw AppError::forbidden();
        }
        return {access.owner_id, access.rel_path};
    }

    if (!user_id)
    {
        throw AppError::unauthorized();
    }

    if (!target.share.empty())
    {
        const Access access =
            resolve_share_access(*user_id, target.share, target.sub_path);
        if (access.permission < need)
        {
            throw AppError::forbidden();
        }
        return {access.owner_id, access.rel_path};
    }

    return {*user_id, workspace::safe_rel_path(target.path)};
}

}  // namespace

void register_routes()
{
    auto &app = drogon::app();

    // ── Owner endpoints: create / list / revoke shares ──

    app.registerHandler(
        "/api/shares",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string owner_id =
                    auth::current_user_id_with_headers(req);
                auto &db = *state().db;

                if (req->method() == drogon::Get)
                {
                    const std::string rel = workspace::safe_rel_path(
                        http::required_query(req, "path"));
                    const auto rows = db.query_all(
                        "SELECT s.id, s.rel_path, s.is_dir, s.permission,"
                        " u.email FROM shares s"
                        " JOIN users u ON u.id = s.grantee_id"
                        " WHERE s.owner_id = ? AND s.rel_path = ?"
                        " ORDER BY u.email",
                        {DbValue(owner_id), DbValue(rel)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        out.push_back({{"id", row.text(0)},
                                       {"relPath", row.text(1)},
                                       {"isDir", row.boolean(2)},
                                       {"permission", row.text(3)},
                                       {"email", row.text(4)}});
                    }
                    return http::json_response(out);
                }

                const auto body = http::json_body(req);
                const auto perm =
                    parse_perm(http::json_string(body, "permission"));
                if (!perm)
                {
                    throw AppError::bad_request(
                        "permission must be view, comment or edit");
                }
                const std::string rel = workspace::safe_rel_path(
                    http::json_string(body, "relPath"));
                const bool is_dir = http::json_bool(body, "isDir");
                require_entry_exists(owner_id, rel, is_dir);

                const std::string email =
                    util::to_lower(util::trim(http::json_string(body, "email")));
                const auto grantee = db.query_one(
                    "SELECT id, email FROM users WHERE email = ?",
                    {DbValue(email)});
                if (!grantee)
                {
                    throw AppError::bad_request("no account with that email");
                }
                const std::string grantee_id = grantee->text(0);
                if (grantee_id == owner_id)
                {
                    throw AppError::bad_request(
                        "that's you — no need to share");
                }

                // Upsert: re-sharing the same entry with the same person
                // updates the level.
                db.execute(
                    "INSERT INTO shares (id, owner_id, grantee_id, rel_path,"
                    " is_dir, permission, created_at)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?)"
                    " ON CONFLICT (owner_id, grantee_id, rel_path)"
                    " DO UPDATE SET permission = excluded.permission,"
                    " is_dir = excluded.is_dir",
                    {DbValue(util::uuid_v4()),
                     DbValue(owner_id),
                     DbValue(grantee_id),
                     DbValue(rel),
                     DbValue(is_dir),
                     DbValue(std::string(perm_str(*perm))),
                     DbValue(util::now_ms())});

                // The upsert may have kept the original row id; read it back.
                const auto actual = db.query_one(
                    "SELECT id FROM shares"
                    " WHERE owner_id = ? AND grantee_id = ? AND rel_path = ?",
                    {DbValue(owner_id), DbValue(grantee_id), DbValue(rel)});
                if (!actual)
                {
                    throw AppError::internal("share row vanished");
                }

                return http::json_response({{"id", actual->text(0)},
                                            {"relPath", rel},
                                            {"isDir", is_dir},
                                            {"email", grantee->text(1)},
                                            {"permission", perm_str(*perm)}});
            });
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/shares/{id}",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string share_id) {
            http::guard(std::move(callback), [&] {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                // Either side can end a share: the owner revokes it, the
                // grantee leaves.
                const int64_t affected = state().db->execute(
                    "DELETE FROM shares"
                    " WHERE id = ? AND (owner_id = ? OR grantee_id = ?)",
                    {DbValue(share_id), DbValue(user_id), DbValue(user_id)});
                if (affected == 0)
                {
                    throw AppError::not_found();
                }
                return http::ok_response();
            });
        },
        {drogon::Delete});

    // ── Link endpoints: owner management ──

    app.registerHandler(
        "/api/links",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string owner_id =
                    auth::current_user_id_with_headers(req);
                auto &db = *state().db;

                if (req->method() == drogon::Get)
                {
                    const std::string rel = workspace::safe_rel_path(
                        http::required_query(req, "path"));
                    const auto rows = db.query_all(
                        "SELECT id, rel_path, is_dir, permission"
                        " FROM share_links"
                        " WHERE owner_id = ? AND rel_path = ?"
                        " ORDER BY created_at",
                        {DbValue(owner_id), DbValue(rel)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        out.push_back({{"id", row.text(0)},
                                       {"relPath", row.text(1)},
                                       {"isDir", row.boolean(2)},
                                       {"permission", row.text(3)}});
                    }
                    return http::json_response(out);
                }

                const auto body = http::json_body(req);
                const auto perm =
                    parse_perm(http::json_string(body, "permission"));
                if (!perm)
                {
                    throw AppError::bad_request(
                        "permission must be view, comment or edit");
                }
                const std::string rel = workspace::safe_rel_path(
                    http::json_string(body, "relPath"));
                const bool is_dir = http::json_bool(body, "isDir");
                require_entry_exists(owner_id, rel, is_dir);

                // The row id doubles as the unguessable URL token, which is
                // the whole capability — no account required to use one.
                const std::string id = util::uuid_v4();
                db.execute(
                    "INSERT INTO share_links (id, owner_id, rel_path, is_dir,"
                    " permission, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(owner_id),
                     DbValue(rel),
                     DbValue(is_dir),
                     DbValue(std::string(perm_str(*perm))),
                     DbValue(util::now_ms())});

                return http::json_response({{"id", id},
                                            {"relPath", rel},
                                            {"isDir", is_dir},
                                            {"permission", perm_str(*perm)}});
            });
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/links/{id}",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string link_id) {
            http::guard(std::move(callback), [&] {
                const std::string owner_id =
                    auth::current_user_id_with_headers(req);
                const int64_t affected = state().db->execute(
                    "DELETE FROM share_links WHERE id = ? AND owner_id = ?",
                    {DbValue(link_id), DbValue(owner_id)});
                if (affected == 0)
                {
                    throw AppError::not_found();
                }
                return http::ok_response();
            });
        },
        {drogon::Delete});

    // ── Link endpoints: anonymous access (the token is the authorization) ──

    app.registerHandler(
        "/api/link/{token}",
        [](const drogon::HttpRequestPtr &,
           HttpCallback &&callback,
           std::string token) {
            http::guard(std::move(callback), [&] {
                const LinkRow link = load_link(token);
                const auto owner = state().db->query_one(
                    "SELECT email FROM users WHERE id = ?",
                    {DbValue(link.owner_id)});
                if (!owner)
                {
                    throw AppError::not_found();
                }

                const fs::path full =
                    owner_full_path(link.owner_id, link.rel_path);
                std::error_code ec;
                // False when the owner has since deleted/moved the linked
                // entry.
                const bool exists = link.is_dir
                                        ? fs::is_directory(full, ec)
                                        : fs::is_regular_file(full, ec);

                return http::json_response(
                    {{"name", std::string(util::basename_of(link.rel_path))},
                     {"isDir", link.is_dir},
                     {"permission", link.permission},
                     {"ownerEmail", owner->text(0)},
                     {"exists", exists && !ec}});
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/link/{token}/list",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string token) {
            http::guard(std::move(callback), [&] {
                const Access access =
                    resolve_link_access(token, http::query(req, "path"));
                if (!access.is_dir)
                {
                    throw AppError::bad_request("not a folder link");
                }
                const fs::path full =
                    owner_full_path(access.owner_id, access.rel_path);
                std::error_code ec;
                if (!fs::is_directory(full, ec) || ec)
                {
                    throw AppError::not_found();
                }
                return http::json_response(workspace::walk_work_dir(full, ""));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/link/{token}/file",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string token) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const Access access =
                    resolve_link_access(token, http::query(req, "path"));
                if (req->method() == drogon::Get)
                {
                    return http::text_response(read_through(access));
                }
                write_through(access, std::string(req->getBody()));
                return http::ok_response();
            });
        },
        {drogon::Get, drogon::Put});

    /// Change-detection poll through a share link (the token is the
    /// authorization).
    app.registerHandler(
        "/api/link/{token}/stat",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string token) {
            http::guard(std::move(callback), [&] {
                const Access access =
                    resolve_link_access(token, http::query(req, "path"));
                if (!workspace::is_work_file(access.rel_path))
                {
                    throw AppError::bad_request("not a work file");
                }
                return http::json_response(workspace::stat_file(
                    owner_full_path(access.owner_id, access.rel_path)));
            });
        },
        {drogon::Get});

    // ── Grantee endpoints: browse and use what's shared with me ──

    app.registerHandler(
        "/api/shared",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                const auto rows = state().db->query_all(
                    "SELECT s.id, s.owner_id, s.rel_path, s.is_dir,"
                    " s.permission, u.email FROM shares s"
                    " JOIN users u ON u.id = s.owner_id"
                    " WHERE s.grantee_id = ? ORDER BY u.email, s.rel_path",
                    {DbValue(user_id)});

                json out = json::array();
                for (const auto &row : rows)
                {
                    const std::string owner_id = row.text(1);
                    const std::string rel_path = row.text(2);
                    const bool is_dir = row.boolean(3);
                    const fs::path full = owner_full_path(owner_id, rel_path);
                    std::error_code ec;
                    const bool exists = is_dir ? fs::is_directory(full, ec)
                                               : fs::is_regular_file(full, ec);

                    out.push_back(
                        {{"shareId", row.text(0)},
                         {"ownerEmail", row.text(5)},
                         {"name", std::string(util::basename_of(rel_path))},
                         {"isDir", is_dir},
                         {"permission", row.text(4)},
                         {"exists", exists && !ec}});
                }
                return http::json_response(out);
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/shared/list",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                const Access access =
                    resolve_share_access(user_id,
                                         http::required_query(req, "share"),
                                         http::query(req, "path"));
                if (!access.is_dir)
                {
                    throw AppError::bad_request("not a folder share");
                }
                const fs::path full =
                    owner_full_path(access.owner_id, access.rel_path);
                std::error_code ec;
                if (!fs::is_directory(full, ec) || ec)
                {
                    throw AppError::not_found();
                }
                return http::json_response(workspace::walk_work_dir(full, ""));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/shared/file",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                const Access access =
                    resolve_share_access(user_id,
                                         http::required_query(req, "share"),
                                         http::query(req, "path"));
                if (req->method() == drogon::Get)
                {
                    return http::text_response(read_through(access));
                }
                write_through(access, std::string(req->getBody()));
                return http::ok_response();
            });
        },
        {drogon::Get, drogon::Put});

    /// Change-detection poll for a shared file; any permission level may
    /// poll, since view access already implies reading.
    app.registerHandler(
        "/api/shared/stat",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&] {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                const Access access =
                    resolve_share_access(user_id,
                                         http::required_query(req, "share"),
                                         http::query(req, "path"));
                if (!workspace::is_work_file(access.rel_path))
                {
                    throw AppError::bad_request("not a work file");
                }
                return http::json_response(workspace::stat_file(
                    owner_full_path(access.owner_id, access.rel_path)));
            });
        },
        {drogon::Get});

    // ── Comments ──

    app.registerHandler(
        "/api/comments",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                auto &st = state();
                const auto user_id = auth::optional_user_id(req);

                if (req->method() == drogon::Get)
                {
                    const auto [owner_id, rel_path] = resolve_comment_target(
                        user_id, target_from_query(req), Perm::View);
                    if (!workspace::is_work_file(rel_path))
                    {
                        throw AppError::bad_request(
                            "comments are only available on work files");
                    }

                    const auto rows = st.db->query_all(
                        "SELECT c.id, u.email, c.body, c.created_at,"
                        " c.author_id, c.anchor FROM comments c"
                        " LEFT JOIN users u ON u.id = c.author_id"
                        " WHERE c.owner_id = ? AND c.rel_path = ?"
                        " ORDER BY c.created_at",
                        {DbValue(owner_id), DbValue(rel_path)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        const auto author_id = row.opt_text(4);
                        const auto author_email = row.opt_text(1);
                        const auto anchor = row.opt_text(5);
                        out.push_back(
                            {{"id", row.text(0)},
                             // Null for comments left by anonymous link
                             // visitors.
                             {"authorEmail",
                              author_email ? json(*author_email) : json()},
                             {"body", row.text(2)},
                             {"createdAt", row.i64(3)},
                             // True when the caller wrote this comment (so
                             // the UI can offer delete).
                             {"mine",
                              author_id.has_value() && author_id == user_id},
                             {"anchor", anchor ? json(*anchor) : json()}});
                    }
                    return http::json_response(out);
                }

                const auto body = http::json_body(req);

                // Anonymous posting is only reachable through comment/edit
                // links; keep a per-IP lid on it since the author can't be
                // held accountable otherwise.
                if (!user_id)
                {
                    const std::string ip = st.client_ip(req);
                    if (st.auth_limiter.is_limited(
                            ip, "anon-comment", kCommentLimit, kCommentWindow))
                    {
                        throw AppError::too_many_requests();
                    }
                    st.auth_limiter.record(ip, "anon-comment", kCommentWindow);
                }

                const auto [owner_id, rel_path] = resolve_comment_target(
                    user_id, target_from_body(body), Perm::Comment);
                if (!workspace::is_work_file(rel_path))
                {
                    throw AppError::bad_request(
                        "comments are only available on work files");
                }

                const std::string text =
                    util::trim(http::json_string(body, "body"));
                if (text.empty())
                {
                    throw AppError::bad_request("empty comment");
                }
                if (text.size() > kMaxCommentLen)
                {
                    throw AppError::bad_request("comment too long");
                }

                const std::string anchor_text =
                    http::json_string_or(body, "anchor");
                const std::optional<std::string> anchor =
                    anchor_text.empty() ? std::nullopt
                                        : std::optional(anchor_text);
                if (anchor && anchor->size() > kMaxAnchorLen)
                {
                    throw AppError::bad_request("anchor too large");
                }

                // The file must actually exist to be commented on.
                std::error_code ec;
                if (!fs::is_regular_file(owner_full_path(owner_id, rel_path),
                                         ec) ||
                    ec)
                {
                    throw AppError::not_found();
                }

                const std::string id = util::uuid_v4();
                const int64_t created_at = util::now_ms();
                st.db->execute(
                    "INSERT INTO comments (id, owner_id, rel_path, author_id,"
                    " body, created_at, anchor) VALUES (?, ?, ?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(owner_id),
                     DbValue(rel_path),
                     user_id ? DbValue(*user_id) : DbValue(std::monostate{}),
                     DbValue(text),
                     DbValue(created_at),
                     anchor ? DbValue(*anchor) : DbValue(std::monostate{})});

                json author_email;
                if (user_id)
                {
                    const auto row = st.db->query_one(
                        "SELECT email FROM users WHERE id = ?",
                        {DbValue(*user_id)});
                    if (row)
                    {
                        author_email = row->text(0);
                    }
                }

                return http::json_response(
                    {{"id", id},
                     {"authorEmail", author_email},
                     {"body", text},
                     {"createdAt", created_at},
                     {"mine", user_id.has_value()},
                     {"anchor", anchor ? json(*anchor) : json()}});
            });
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/comments/{id}",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string comment_id) {
            http::guard(std::move(callback), [&] {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                // The comment's author or the file's owner may delete it.
                const int64_t affected = state().db->execute(
                    "DELETE FROM comments"
                    " WHERE id = ? AND (author_id = ? OR owner_id = ?)",
                    {DbValue(comment_id), DbValue(user_id), DbValue(user_id)});
                if (affected == 0)
                {
                    throw AppError::not_found();
                }
                return http::ok_response();
            });
        },
        {drogon::Delete});
}

}  // namespace lectern::share
