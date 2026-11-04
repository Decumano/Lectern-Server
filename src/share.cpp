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

constexpr size_t cMaxCommentLen = 4000;
/// Anchors are small client-defined JSON blobs (text range / cell range /
/// item id); the server only bounds their size.
constexpr size_t cMaxAnchorLen = 2000;

// Anonymous link visitors can post comments; cap the rate per IP so a leaked
// comment-link can't be used to flood a document.
constexpr uint32_t cCommentLimit = 20;
constexpr std::chrono::seconds cCommentWindow = 10min;

/// Ordered so `perm < Perm::Edit` reads the same as it did in Rust.
enum class Perm
{
    View = 0,
    Comment = 1,
    Edit = 2,
};

std::optional<Perm> fParsePerm(const std::string &value)
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

const char *fPermStr(Perm perm)
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

Perm fPermFromDb(const std::string &value)
{
    const auto parsed = fParsePerm(value);
    if (!parsed)
    {
        throw AppError::fInternal("bad permission in db");
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

ShareRow fLoadShare(const std::string &share_id)
{
    const auto row = fState().db->fQueryOne(
        "SELECT owner_id, grantee_id, rel_path, is_dir, permission"
        " FROM shares WHERE id = ?",
        {DbValue(share_id)});
    if (!row)
    {
        throw AppError::fNotFound();
    }
    return {row->fText(0),
            row->fText(1),
            row->fText(2),
            row->fBoolean(3),
            row->fText(4)};
}

LinkRow fLoadLink(const std::string &token)
{
    const auto row = fState().db->fQueryOne(
        "SELECT owner_id, rel_path, is_dir, permission FROM share_links"
        " WHERE id = ?",
        {DbValue(token)});
    if (!row)
    {
        throw AppError::fNotFound();
    }
    return {row->fText(0), row->fText(1), row->fBoolean(2), row->fText(3)};
}

/// Joins the optional sub path (folder shares only) safely under the shared
/// root, giving the owner-workspace-relative path.
std::string fEffectivePath(const std::string &shared_root,
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
        throw AppError::fBadRequest(not_a_folder_message);
    }
    return shared_root + "/" + workspace::fSafeRelPath(sub_path);
}

/// Resolves a grantee's access through a share: checks the share belongs to
/// this user, then joins any sub path under the shared root.
Access fResolveShareAccess(const std::string &user_id,
                            const std::string &share_id,
                            const std::string &sub_path)
{
    const ShareRow share = fLoadShare(share_id);
    if (share.grantee_id != user_id)
    {
        throw AppError::fNotFound();  // don't reveal that the share exists
    }
    return {share.owner_id,
            fEffectivePath(share.rel_path,
                           share.is_dir,
                           sub_path,
                           "not a folder share"),
            share.is_dir,
            fPermFromDb(share.permission)};
}

/// Same shape as `resolve_share_access`, but the token itself is the
/// authorization — no session or account involved.
Access fResolveLinkAccess(const std::string &token,
                           const std::string &sub_path)
{
    const LinkRow link = fLoadLink(token);
    return {link.owner_id,
            fEffectivePath(link.rel_path,
                           link.is_dir,
                           sub_path,
                           "not a folder link"),
            link.is_dir,
            fPermFromDb(link.permission)};
}

fs::path fOwnerFullPath(const std::string &owner_id, const std::string &rel)
{
    // rel comes from the shares table (owner-created) plus a safe_rel_path'd
    // sub path, but re-validate the whole thing anyway before touching disk.
    return fState().workspaces_dir / owner_id / workspace::fSafeRelPath(rel);
}

/// Shared read of a work file through either a share or a link.
std::string fReadThrough(const Access &access)
{
    if (!workspace::fIsWorkFile(access.rel_path))
    {
        throw AppError::fBadRequest("not a work file");
    }
    const auto content =
        util::fReadFile(fOwnerFullPath(access.owner_id, access.rel_path));
    if (!content)
    {
        throw AppError::fNotFound();
    }
    return *content;
}

/// Shared write of a work file through either a share or a link. Grantees can
/// edit existing files but not create new ones in the owner's workspace;
/// keeps an edit share from being used to fill someone's disk. Writes land in
/// the owner's workspace, so they count against the owner's quota — otherwise
/// an edit share is a way around it.
void fWriteThrough(const Access &access, const std::string &content)
{
    if (access.permission < Perm::Edit)
    {
        throw AppError::fForbidden();
    }
    if (!workspace::fIsWorkFile(access.rel_path))
    {
        throw AppError::fBadRequest("not a work file");
    }

    const fs::path full = fOwnerFullPath(access.owner_id, access.rel_path);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec) || ec)
    {
        throw AppError::fNotFound();
    }

    const fs::path owner_root = fState().workspaces_dir / access.owner_id;
    workspace::fCheckQuota(owner_root, full, content.size());
    util::fWriteFileAtomic(full, content);
}

/// Confirms the entry the owner is about to share actually exists.
void fRequireEntryExists(const std::string &owner_id,
                          const std::string &rel,
                          bool is_dir)
{
    const fs::path full = fState().workspaces_dir / owner_id / rel;
    std::error_code ec;
    if (is_dir && (!fs::is_directory(full, ec) || ec))
    {
        throw AppError::fBadRequest("no such folder");
    }
    if (!is_dir && (!fs::is_regular_file(full, ec) || ec))
    {
        throw AppError::fBadRequest("no such file");
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

CommentTarget fTargetFromQuery(const drogon::HttpRequestPtr &req)
{
    return {http::fQuery(req, "path"),
            http::fQuery(req, "share"),
            http::fQuery(req, "link"),
            http::fQuery(req, "subPath")};
}

CommentTarget fTargetFromBody(const json &body)
{
    return {http::fJsonStringOr(body, "path"),
            http::fJsonStringOr(body, "share"),
            http::fJsonStringOr(body, "link"),
            http::fJsonStringOr(body, "subPath")};
}

/// Works out which file a comments request is about and whether the caller
/// may see / post at the required level. `user_id` is nullopt for anonymous
/// link visitors, which is only acceptable in the link context.
std::pair<std::string, std::string> fResolveCommentTarget(
    const std::optional<std::string> &user_id,
    const CommentTarget &target,
    Perm need)
{
    if (!target.link.empty())
    {
        const Access access =
            fResolveLinkAccess(target.link, target.sub_path);
        if (access.permission < need)
        {
            throw AppError::fForbidden();
        }
        return {access.owner_id, access.rel_path};
    }

    if (!user_id)
    {
        throw AppError::fUnauthorized();
    }

    if (!target.share.empty())
    {
        const Access access =
            fResolveShareAccess(*user_id, target.share, target.sub_path);
        if (access.permission < need)
        {
            throw AppError::fForbidden();
        }
        return {access.owner_id, access.rel_path};
    }

    return {*user_id, workspace::fSafeRelPath(target.path)};
}

}  // namespace

void fRegisterRoutes()
{
    auto &app = drogon::app();

    // ── Owner endpoints: create / list / revoke shares ──

    app.registerHandler(
        "/api/shares",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string owner_id =
                    auth::fCurrentUserIdWithHeaders(req);
                auto &db = *fState().db;

                if (req->method() == drogon::Get)
                {
                    const std::string rel = workspace::fSafeRelPath(
                        http::fRequiredQuery(req, "path"));
                    const auto rows = db.fQueryAll(
                        "SELECT s.id, s.rel_path, s.is_dir, s.permission,"
                        " u.email FROM shares s"
                        " JOIN users u ON u.id = s.grantee_id"
                        " WHERE s.owner_id = ? AND s.rel_path = ?"
                        " ORDER BY u.email",
                        {DbValue(owner_id), DbValue(rel)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        out.push_back({{"id", row.fText(0)},
                                       {"relPath", row.fText(1)},
                                       {"isDir", row.fBoolean(2)},
                                       {"permission", row.fText(3)},
                                       {"email", row.fText(4)}});
                    }
                    return http::fJsonResponse(out);
                }

                const auto body = http::fJsonBody(req);
                const auto perm =
                    fParsePerm(http::fJsonString(body, "permission"));
                if (!perm)
                {
                    throw AppError::fBadRequest(
                        "permission must be view, comment or edit");
                }
                const std::string rel = workspace::fSafeRelPath(
                    http::fJsonString(body, "relPath"));
                const bool is_dir = http::fJsonBool(body, "isDir");
                fRequireEntryExists(owner_id, rel, is_dir);

                const std::string email =
                    util::fToLower(util::fTrim(http::fJsonString(body, "email")));
                const auto grantee = db.fQueryOne(
                    "SELECT id, email FROM users WHERE email = ?",
                    {DbValue(email)});
                if (!grantee)
                {
                    throw AppError::fBadRequest("no account with that email");
                }
                const std::string grantee_id = grantee->fText(0);
                if (grantee_id == owner_id)
                {
                    throw AppError::fBadRequest(
                        "that's you — no need to share");
                }

                // Upsert: re-sharing the same entry with the same person
                // updates the level.
                db.fExecute(
                    "INSERT INTO shares (id, owner_id, grantee_id, rel_path,"
                    " is_dir, permission, created_at)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?)"
                    " ON CONFLICT (owner_id, grantee_id, rel_path)"
                    " DO UPDATE SET permission = excluded.permission,"
                    " is_dir = excluded.is_dir",
                    {DbValue(util::fUuidV4()),
                     DbValue(owner_id),
                     DbValue(grantee_id),
                     DbValue(rel),
                     DbValue(is_dir),
                     DbValue(std::string(fPermStr(*perm))),
                     DbValue(util::fNowMs())});

                // The upsert may have kept the original row id; read it back.
                const auto actual = db.fQueryOne(
                    "SELECT id FROM shares"
                    " WHERE owner_id = ? AND grantee_id = ? AND rel_path = ?",
                    {DbValue(owner_id), DbValue(grantee_id), DbValue(rel)});
                if (!actual)
                {
                    throw AppError::fInternal("share row vanished");
                }

                return http::fJsonResponse({{"id", actual->fText(0)},
                                            {"relPath", rel},
                                            {"isDir", is_dir},
                                            {"email", grantee->fText(1)},
                                            {"permission", fPermStr(*perm)}});
            });
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/shares/{id}",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string share_id) {
            http::fGuard(std::move(callback), [&] {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                // Either side can end a share: the owner revokes it, the
                // grantee leaves.
                const int64_t affected = fState().db->fExecute(
                    "DELETE FROM shares"
                    " WHERE id = ? AND (owner_id = ? OR grantee_id = ?)",
                    {DbValue(share_id), DbValue(user_id), DbValue(user_id)});
                if (affected == 0)
                {
                    throw AppError::fNotFound();
                }
                return http::fOkResponse();
            });
        },
        {drogon::Delete});

    // ── Link endpoints: owner management ──

    app.registerHandler(
        "/api/links",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string owner_id =
                    auth::fCurrentUserIdWithHeaders(req);
                auto &db = *fState().db;

                if (req->method() == drogon::Get)
                {
                    const std::string rel = workspace::fSafeRelPath(
                        http::fRequiredQuery(req, "path"));
                    const auto rows = db.fQueryAll(
                        "SELECT id, rel_path, is_dir, permission"
                        " FROM share_links"
                        " WHERE owner_id = ? AND rel_path = ?"
                        " ORDER BY created_at",
                        {DbValue(owner_id), DbValue(rel)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        out.push_back({{"id", row.fText(0)},
                                       {"relPath", row.fText(1)},
                                       {"isDir", row.fBoolean(2)},
                                       {"permission", row.fText(3)}});
                    }
                    return http::fJsonResponse(out);
                }

                const auto body = http::fJsonBody(req);
                const auto perm =
                    fParsePerm(http::fJsonString(body, "permission"));
                if (!perm)
                {
                    throw AppError::fBadRequest(
                        "permission must be view, comment or edit");
                }
                const std::string rel = workspace::fSafeRelPath(
                    http::fJsonString(body, "relPath"));
                const bool is_dir = http::fJsonBool(body, "isDir");
                fRequireEntryExists(owner_id, rel, is_dir);

                // The row id doubles as the unguessable URL token, which is
                // the whole capability — no account required to use one.
                const std::string id = util::fUuidV4();
                db.fExecute(
                    "INSERT INTO share_links (id, owner_id, rel_path, is_dir,"
                    " permission, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(owner_id),
                     DbValue(rel),
                     DbValue(is_dir),
                     DbValue(std::string(fPermStr(*perm))),
                     DbValue(util::fNowMs())});

                return http::fJsonResponse({{"id", id},
                                            {"relPath", rel},
                                            {"isDir", is_dir},
                                            {"permission", fPermStr(*perm)}});
            });
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/links/{id}",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string link_id) {
            http::fGuard(std::move(callback), [&] {
                const std::string owner_id =
                    auth::fCurrentUserIdWithHeaders(req);
                const int64_t affected = fState().db->fExecute(
                    "DELETE FROM share_links WHERE id = ? AND owner_id = ?",
                    {DbValue(link_id), DbValue(owner_id)});
                if (affected == 0)
                {
                    throw AppError::fNotFound();
                }
                return http::fOkResponse();
            });
        },
        {drogon::Delete});

    // ── Link endpoints: anonymous access (the token is the authorization) ──

    app.registerHandler(
        "/api/link/{token}",
        [](const drogon::HttpRequestPtr &,
           HttpCallback &&callback,
           std::string token) {
            http::fGuard(std::move(callback), [&] {
                const LinkRow link = fLoadLink(token);
                const auto owner = fState().db->fQueryOne(
                    "SELECT email FROM users WHERE id = ?",
                    {DbValue(link.owner_id)});
                if (!owner)
                {
                    throw AppError::fNotFound();
                }

                const fs::path full =
                    fOwnerFullPath(link.owner_id, link.rel_path);
                std::error_code ec;
                // False when the owner has since deleted/moved the linked
                // entry.
                const bool exists = link.is_dir
                                        ? fs::is_directory(full, ec)
                                        : fs::is_regular_file(full, ec);

                return http::fJsonResponse(
                    {{"name", std::string(util::fBasenameOf(link.rel_path))},
                     {"isDir", link.is_dir},
                     {"permission", link.permission},
                     {"ownerEmail", owner->fText(0)},
                     {"exists", exists && !ec}});
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/link/{token}/list",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string token) {
            http::fGuard(std::move(callback), [&] {
                const Access access =
                    fResolveLinkAccess(token, http::fQuery(req, "path"));
                if (!access.is_dir)
                {
                    throw AppError::fBadRequest("not a folder link");
                }
                const fs::path full =
                    fOwnerFullPath(access.owner_id, access.rel_path);
                std::error_code ec;
                if (!fs::is_directory(full, ec) || ec)
                {
                    throw AppError::fNotFound();
                }
                return http::fJsonResponse(workspace::fWalkWorkDir(full, ""));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/link/{token}/file",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string token) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const Access access =
                    fResolveLinkAccess(token, http::fQuery(req, "path"));
                if (req->method() == drogon::Get)
                {
                    return http::fTextResponse(fReadThrough(access));
                }
                fWriteThrough(access, std::string(req->getBody()));
                return http::fOkResponse();
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
            http::fGuard(std::move(callback), [&] {
                const Access access =
                    fResolveLinkAccess(token, http::fQuery(req, "path"));
                if (!workspace::fIsWorkFile(access.rel_path))
                {
                    throw AppError::fBadRequest("not a work file");
                }
                return http::fJsonResponse(workspace::fStatFile(
                    fOwnerFullPath(access.owner_id, access.rel_path)));
            });
        },
        {drogon::Get});

    // ── Grantee endpoints: browse and use what's shared with me ──

    app.registerHandler(
        "/api/shared",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                const auto rows = fState().db->fQueryAll(
                    "SELECT s.id, s.owner_id, s.rel_path, s.is_dir,"
                    " s.permission, u.email FROM shares s"
                    " JOIN users u ON u.id = s.owner_id"
                    " WHERE s.grantee_id = ? ORDER BY u.email, s.rel_path",
                    {DbValue(user_id)});

                json out = json::array();
                for (const auto &row : rows)
                {
                    const std::string owner_id = row.fText(1);
                    const std::string rel_path = row.fText(2);
                    const bool is_dir = row.fBoolean(3);
                    const fs::path full = fOwnerFullPath(owner_id, rel_path);
                    std::error_code ec;
                    const bool exists = is_dir ? fs::is_directory(full, ec)
                                               : fs::is_regular_file(full, ec);

                    out.push_back(
                        {{"shareId", row.fText(0)},
                         {"ownerEmail", row.fText(5)},
                         {"name", std::string(util::fBasenameOf(rel_path))},
                         {"isDir", is_dir},
                         {"permission", row.fText(4)},
                         {"exists", exists && !ec}});
                }
                return http::fJsonResponse(out);
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/shared/list",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                const Access access =
                    fResolveShareAccess(user_id,
                                         http::fRequiredQuery(req, "share"),
                                         http::fQuery(req, "path"));
                if (!access.is_dir)
                {
                    throw AppError::fBadRequest("not a folder share");
                }
                const fs::path full =
                    fOwnerFullPath(access.owner_id, access.rel_path);
                std::error_code ec;
                if (!fs::is_directory(full, ec) || ec)
                {
                    throw AppError::fNotFound();
                }
                return http::fJsonResponse(workspace::fWalkWorkDir(full, ""));
            });
        },
        {drogon::Get});

    app.registerHandler(
        "/api/shared/file",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                const Access access =
                    fResolveShareAccess(user_id,
                                         http::fRequiredQuery(req, "share"),
                                         http::fQuery(req, "path"));
                if (req->method() == drogon::Get)
                {
                    return http::fTextResponse(fReadThrough(access));
                }
                fWriteThrough(access, std::string(req->getBody()));
                return http::fOkResponse();
            });
        },
        {drogon::Get, drogon::Put});

    /// Change-detection poll for a shared file; any permission level may
    /// poll, since view access already implies reading.
    app.registerHandler(
        "/api/shared/stat",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&] {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                const Access access =
                    fResolveShareAccess(user_id,
                                         http::fRequiredQuery(req, "share"),
                                         http::fQuery(req, "path"));
                if (!workspace::fIsWorkFile(access.rel_path))
                {
                    throw AppError::fBadRequest("not a work file");
                }
                return http::fJsonResponse(workspace::fStatFile(
                    fOwnerFullPath(access.owner_id, access.rel_path)));
            });
        },
        {drogon::Get});

    // ── Comments ──

    app.registerHandler(
        "/api/comments",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                auto &st = fState();
                const auto user_id = auth::fOptionalUserId(req);

                if (req->method() == drogon::Get)
                {
                    const auto [owner_id, rel_path] = fResolveCommentTarget(
                        user_id, fTargetFromQuery(req), Perm::View);
                    if (!workspace::fIsWorkFile(rel_path))
                    {
                        throw AppError::fBadRequest(
                            "comments are only available on work files");
                    }

                    const auto rows = st.db->fQueryAll(
                        "SELECT c.id, u.email, c.body, c.created_at,"
                        " c.author_id, c.anchor FROM comments c"
                        " LEFT JOIN users u ON u.id = c.author_id"
                        " WHERE c.owner_id = ? AND c.rel_path = ?"
                        " ORDER BY c.created_at",
                        {DbValue(owner_id), DbValue(rel_path)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        const auto author_id = row.fOptText(4);
                        const auto author_email = row.fOptText(1);
                        const auto anchor = row.fOptText(5);
                        out.push_back(
                            {{"id", row.fText(0)},
                             // Null for comments left by anonymous link
                             // visitors.
                             {"authorEmail",
                              author_email ? json(*author_email) : json()},
                             {"body", row.fText(2)},
                             {"createdAt", row.fI64(3)},
                             // True when the caller wrote this comment (so
                             // the UI can offer delete).
                             {"mine",
                              author_id.has_value() && author_id == user_id},
                             {"anchor", anchor ? json(*anchor) : json()}});
                    }
                    return http::fJsonResponse(out);
                }

                const auto body = http::fJsonBody(req);

                // Anonymous posting is only reachable through comment/edit
                // links; keep a per-IP lid on it since the author can't be
                // held accountable otherwise.
                if (!user_id)
                {
                    const std::string ip = st.fClientIp(req);
                    if (st.auth_limiter.fIsLimited(
                            ip, "anon-comment", cCommentLimit, cCommentWindow))
                    {
                        throw AppError::fTooManyRequests();
                    }
                    st.auth_limiter.fRecord(ip, "anon-comment", cCommentWindow);
                }

                const auto [owner_id, rel_path] = fResolveCommentTarget(
                    user_id, fTargetFromBody(body), Perm::Comment);
                if (!workspace::fIsWorkFile(rel_path))
                {
                    throw AppError::fBadRequest(
                        "comments are only available on work files");
                }

                const std::string text =
                    util::fTrim(http::fJsonString(body, "body"));
                if (text.empty())
                {
                    throw AppError::fBadRequest("empty comment");
                }
                if (text.size() > cMaxCommentLen)
                {
                    throw AppError::fBadRequest("comment too long");
                }

                const std::string anchor_text =
                    http::fJsonStringOr(body, "anchor");
                const std::optional<std::string> anchor =
                    anchor_text.empty() ? std::nullopt
                                        : std::optional(anchor_text);
                if (anchor && anchor->size() > cMaxAnchorLen)
                {
                    throw AppError::fBadRequest("anchor too large");
                }

                // The file must actually exist to be commented on.
                std::error_code ec;
                if (!fs::is_regular_file(fOwnerFullPath(owner_id, rel_path),
                                         ec) ||
                    ec)
                {
                    throw AppError::fNotFound();
                }

                const std::string id = util::fUuidV4();
                const int64_t created_at = util::fNowMs();
                st.db->fExecute(
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
                    const auto row = st.db->fQueryOne(
                        "SELECT email FROM users WHERE id = ?",
                        {DbValue(*user_id)});
                    if (row)
                    {
                        author_email = row->fText(0);
                    }
                }

                return http::fJsonResponse(
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
            http::fGuard(std::move(callback), [&] {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                // The comment's author or the file's owner may delete it.
                const int64_t affected = fState().db->fExecute(
                    "DELETE FROM comments"
                    " WHERE id = ? AND (author_id = ? OR owner_id = ?)",
                    {DbValue(comment_id), DbValue(user_id), DbValue(user_id)});
                if (affected == 0)
                {
                    throw AppError::fNotFound();
                }
                return http::fOkResponse();
            });
        },
        {drogon::Delete});
}

}  // namespace lectern::share
