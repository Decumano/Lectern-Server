#include "fonts.h"

#include <drogon/HttpAppFramework.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <optional>

#include "auth.h"
#include "error.h"
#include "http.h"
#include "state.h"
#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lectern::fonts {

namespace {

constexpr int64_t cMaxFontsPerAccount = 30;

std::optional<std::string> fContentTypeFor(const std::string &ext)
{
    if (ext == "ttf")
    {
        return "font/ttf";
    }
    if (ext == "otf")
    {
        return "font/otf";
    }
    if (ext == "woff")
    {
        return "font/woff";
    }
    if (ext == "woff2")
    {
        return "font/woff2";
    }
    return std::nullopt;
}

/// The stored content type is `font/<ext>`; recover the extension from it.
std::string fExtFromContentType(const std::string &content_type)
{
    const size_t slash = content_type.rfind('/');
    if (slash == std::string::npos || slash + 1 >= content_type.size())
    {
        return "ttf";
    }
    return content_type.substr(slash + 1);
}

fs::path fFontFilePath(const std::string &owner_id,
                        const std::string &font_id,
                        const std::string &ext)
{
    return fState().fonts_dir / owner_id / (font_id + "." + ext);
}

/// Characters that could escape a single-quoted CSS string, or the enclosing
/// `<style>` element, when a family name is interpolated into either.
bool fIsUnsafeFamilyChar(char c)
{
    const auto uc = static_cast<unsigned char>(c);
    if (std::iscntrl(uc) != 0)
    {
        return true;
    }
    switch (c)
    {
        case '\'':
        case '"':
        case '<':
        case '>':
        case '\\':
        case '{':
        case '}':
        case ';':
        case '&':
            return true;
        default:
            return false;
    }
}

/// Sanitizes a user-supplied font family name for use inside a single-quoted
/// CSS string that itself sits inside an HTML `<style>` element. Dropping
/// only `'` isn't enough: `<` would let a family name close the style element
/// early (`</style><script>…`), and a raw newline or backslash can break out
/// of the CSS string. Keep it to characters that can only ever be text.
///
/// Upload already rejects these characters, so in practice this never has to
/// remove anything — it's the second of the two locks on the same door.
std::string fCssStringLiteralBody(const std::string &family_name)
{
    std::string out;
    out.reserve(family_name.size());
    for (const char c : family_name)
    {
        if (!fIsUnsafeFamilyChar(c))
        {
            out.push_back(c);
        }
    }
    return out;
}

const char *fFontFormatFor(const std::string &ext)
{
    if (ext == "woff2")
    {
        return "woff2";
    }
    if (ext == "woff")
    {
        return "woff";
    }
    if (ext == "otf")
    {
        return "opentype";
    }
    return "truetype";
}

struct FontRow
{
    std::string owner_id;
    std::string content_type;
};

FontRow fLoadFontRow(const std::string &font_id)
{
    const auto row = fState().db->fQueryOne(
        "SELECT owner_id, content_type FROM custom_fonts WHERE id = ?",
        {DbValue(font_id)});
    if (!row)
    {
        throw AppError::fNotFound();
    }
    return {row->fText(0), row->fText(1)};
}

/// The id becomes a filesystem path segment and a URL path segment; keep it
/// to the UUID alphabet so a hostile value can't reach either.
void fValidateFontId(const std::string &font_id)
{
    if (font_id.empty() || font_id.size() > 64)
    {
        throw AppError::fBadRequest("invalid font id");
    }
    for (const char c : font_id)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '-')
        {
            throw AppError::fBadRequest("invalid font id");
        }
    }
}

}  // namespace

std::string fEmbedAccountFontsStyle(const std::string &owner_id)
{
    std::vector<Row> rows;
    try
    {
        rows = fState().db->fQueryAll(
            "SELECT id, family_name, content_type FROM custom_fonts"
            " WHERE owner_id = ?",
            {DbValue(owner_id)});
    }
    catch (const std::exception &)
    {
        return {};  // a font lookup failure must not fail the export
    }

    if (rows.empty())
    {
        return {};
    }

    std::string style = "<style>\n";
    for (const auto &row : rows)
    {
        const std::string id = row.fText(0);
        const std::string family_name = row.fText(1);
        const std::string content_type = row.fText(2);
        const std::string ext = fExtFromContentType(content_type);

        const auto bytes = util::fReadFile(fFontFilePath(owner_id, id, ext));
        if (!bytes)
        {
            continue;
        }

        style += "@font-face{font-family:'";
        style += fCssStringLiteralBody(family_name);
        style += "';src:url(data:";
        style += content_type;
        style += ";base64,";
        style += util::fBase64Encode(*bytes);
        style += ") format('";
        style += fFontFormatFor(ext);
        style += "');}\n";
    }
    style += "</style>\n";
    return style;
}

void fRegisterRoutes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/fonts",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                auto &db = *fState().db;

                if (req->method() == drogon::Get)
                {
                    const auto rows = db.fQueryAll(
                        "SELECT id, family_name, content_type, byte_size"
                        " FROM custom_fonts WHERE owner_id = ?"
                        " ORDER BY family_name",
                        {DbValue(user_id)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        out.push_back({{"id", row.fText(0)},
                                       {"familyName", row.fText(1)},
                                       {"contentType", row.fText(2)},
                                       {"byteSize", row.fI64(3)}});
                    }
                    return http::fJsonResponse(out);
                }

                const std::string family_name =
                    util::fTrim(http::fRequiredQuery(req, "familyName"));
                const std::string filename =
                    http::fRequiredQuery(req, "filename");

                // The name is interpolated into a CSS string inside a
                // `<style>` element for PDF export (see
                // embed_account_fonts_style); reject anything that could
                // escape either context up front, so what's stored is always
                // safe to emit verbatim.
                bool bad_name = family_name.empty() || family_name.size() > 100;
                for (const char c : family_name)
                {
                    bad_name = bad_name || fIsUnsafeFamilyChar(c);
                }
                if (bad_name)
                {
                    throw AppError::fBadRequest("invalid font family name");
                }

                const std::string ext = util::fExtensionOf(filename);
                const auto content_type = fContentTypeFor(ext);
                if (!content_type)
                {
                    throw AppError::fBadRequest(
                        "font must be .ttf, .otf, .woff or .woff2");
                }

                const std::string body(req->getBody());
                if (body.empty() || body.size() > cMaxFontBytes)
                {
                    throw AppError::fBadRequest(
                        "font file too large (5MB max)");
                }

                const auto count = db.fQueryOne(
                    "SELECT COUNT(*) FROM custom_fonts WHERE owner_id = ?",
                    {DbValue(user_id)});
                if (count && count->fI64(0) >= cMaxFontsPerAccount)
                {
                    throw AppError::fBadRequest(
                        "font library is full (" +
                        std::to_string(cMaxFontsPerAccount) +
                        " max) — delete one first");
                }

                const std::string id = util::fUuidV4();
                util::fWriteFileAtomic(fFontFilePath(user_id, id, ext),
                                        body);

                // Re-uploading an existing family name replaces the file; the
                // UNIQUE constraint means the insert would otherwise fail
                // outright.
                const auto existing = db.fQueryOne(
                    "SELECT id, content_type FROM custom_fonts"
                    " WHERE owner_id = ? AND family_name = ?",
                    {DbValue(user_id), DbValue(family_name)});
                if (existing)
                {
                    const std::string old_id = existing->fText(0);
                    std::error_code ec;
                    fs::remove(
                        fFontFilePath(user_id,
                                       old_id,
                                       fExtFromContentType(
                                           existing->fText(1))),
                        ec);
                    db.fExecute("DELETE FROM custom_fonts WHERE id = ?",
                               {DbValue(old_id)});
                }

                db.fExecute(
                    "INSERT INTO custom_fonts (id, owner_id, family_name,"
                    " filename, content_type, byte_size, created_at)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(user_id),
                     DbValue(family_name),
                     DbValue(filename),
                     DbValue(*content_type),
                     DbValue(static_cast<int64_t>(body.size())),
                     DbValue(util::fNowMs())});

                return http::fJsonResponse(
                    {{"id", id},
                     {"familyName", family_name},
                     {"contentType", *content_type},
                     {"byteSize", static_cast<int64_t>(body.size())}});
            });
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/fonts/{id}",
        [](const drogon::HttpRequestPtr &req,
           HttpCallback &&callback,
           std::string font_id) {
            http::fGuard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string user_id =
                    auth::fCurrentUserIdWithHeaders(req);
                fValidateFontId(font_id);
                const FontRow row = fLoadFontRow(font_id);
                if (row.owner_id != user_id)
                {
                    throw AppError::fNotFound();
                }
                const std::string ext = fExtFromContentType(row.content_type);

                if (req->method() == drogon::Delete)
                {
                    std::error_code ec;
                    fs::remove(fFontFilePath(user_id, font_id, ext), ec);
                    fState().db->fExecute(
                        "DELETE FROM custom_fonts WHERE id = ?",
                        {DbValue(font_id)});
                    return http::fOkResponse();
                }

                const auto bytes =
                    util::fReadFile(fFontFilePath(user_id, font_id, ext));
                if (!bytes)
                {
                    throw AppError::fNotFound();
                }

                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM,
                                                        row.content_type);
                // The font list rarely changes; the app re-injects @font-face
                // on every load anyway, so a modest cache just saves repeat
                // fetches.
                resp->addHeader("Cache-Control", "private, max-age=3600");
                resp->setBody(*bytes);
                return resp;
            });
        },
        {drogon::Get, drogon::Delete});
}

}  // namespace lectern::fonts
