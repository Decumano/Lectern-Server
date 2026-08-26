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

constexpr int64_t kMaxFontsPerAccount = 30;

std::optional<std::string> content_type_for(const std::string &ext)
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
std::string ext_from_content_type(const std::string &content_type)
{
    const size_t slash = content_type.rfind('/');
    if (slash == std::string::npos || slash + 1 >= content_type.size())
    {
        return "ttf";
    }
    return content_type.substr(slash + 1);
}

fs::path font_file_path(const std::string &owner_id,
                        const std::string &font_id,
                        const std::string &ext)
{
    return state().fonts_dir / owner_id / (font_id + "." + ext);
}

/// Characters that could escape a single-quoted CSS string, or the enclosing
/// `<style>` element, when a family name is interpolated into either.
bool is_unsafe_family_char(char c)
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
std::string css_string_literal_body(const std::string &family_name)
{
    std::string out;
    out.reserve(family_name.size());
    for (const char c : family_name)
    {
        if (!is_unsafe_family_char(c))
        {
            out.push_back(c);
        }
    }
    return out;
}

const char *font_format_for(const std::string &ext)
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

FontRow load_font_row(const std::string &font_id)
{
    const auto row = state().db->query_one(
        "SELECT owner_id, content_type FROM custom_fonts WHERE id = ?",
        {DbValue(font_id)});
    if (!row)
    {
        throw AppError::not_found();
    }
    return {row->text(0), row->text(1)};
}

/// The id becomes a filesystem path segment and a URL path segment; keep it
/// to the UUID alphabet so a hostile value can't reach either.
void validate_font_id(const std::string &font_id)
{
    if (font_id.empty() || font_id.size() > 64)
    {
        throw AppError::bad_request("invalid font id");
    }
    for (const char c : font_id)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '-')
        {
            throw AppError::bad_request("invalid font id");
        }
    }
}

}  // namespace

std::string embed_account_fonts_style(const std::string &owner_id)
{
    std::vector<Row> rows;
    try
    {
        rows = state().db->query_all(
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
        const std::string id = row.text(0);
        const std::string family_name = row.text(1);
        const std::string content_type = row.text(2);
        const std::string ext = ext_from_content_type(content_type);

        const auto bytes = util::read_file(font_file_path(owner_id, id, ext));
        if (!bytes)
        {
            continue;
        }

        style += "@font-face{font-family:'";
        style += css_string_literal_body(family_name);
        style += "';src:url(data:";
        style += content_type;
        style += ";base64,";
        style += util::base64_encode(*bytes);
        style += ") format('";
        style += font_format_for(ext);
        style += "');}\n";
    }
    style += "</style>\n";
    return style;
}

void register_routes()
{
    auto &app = drogon::app();

    app.registerHandler(
        "/api/fonts",
        [](const drogon::HttpRequestPtr &req, HttpCallback &&callback) {
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                auto &db = *state().db;

                if (req->method() == drogon::Get)
                {
                    const auto rows = db.query_all(
                        "SELECT id, family_name, content_type, byte_size"
                        " FROM custom_fonts WHERE owner_id = ?"
                        " ORDER BY family_name",
                        {DbValue(user_id)});

                    json out = json::array();
                    for (const auto &row : rows)
                    {
                        out.push_back({{"id", row.text(0)},
                                       {"familyName", row.text(1)},
                                       {"contentType", row.text(2)},
                                       {"byteSize", row.i64(3)}});
                    }
                    return http::json_response(out);
                }

                const std::string family_name =
                    util::trim(http::required_query(req, "familyName"));
                const std::string filename =
                    http::required_query(req, "filename");

                // The name is interpolated into a CSS string inside a
                // `<style>` element for PDF export (see
                // embed_account_fonts_style); reject anything that could
                // escape either context up front, so what's stored is always
                // safe to emit verbatim.
                bool bad_name = family_name.empty() || family_name.size() > 100;
                for (const char c : family_name)
                {
                    bad_name = bad_name || is_unsafe_family_char(c);
                }
                if (bad_name)
                {
                    throw AppError::bad_request("invalid font family name");
                }

                const std::string ext = util::extension_of(filename);
                const auto content_type = content_type_for(ext);
                if (!content_type)
                {
                    throw AppError::bad_request(
                        "font must be .ttf, .otf, .woff or .woff2");
                }

                const std::string body(req->getBody());
                if (body.empty() || body.size() > kMaxFontBytes)
                {
                    throw AppError::bad_request(
                        "font file too large (5MB max)");
                }

                const auto count = db.query_one(
                    "SELECT COUNT(*) FROM custom_fonts WHERE owner_id = ?",
                    {DbValue(user_id)});
                if (count && count->i64(0) >= kMaxFontsPerAccount)
                {
                    throw AppError::bad_request(
                        "font library is full (" +
                        std::to_string(kMaxFontsPerAccount) +
                        " max) — delete one first");
                }

                const std::string id = util::uuid_v4();
                util::write_file_atomic(font_file_path(user_id, id, ext),
                                        body);

                // Re-uploading an existing family name replaces the file; the
                // UNIQUE constraint means the insert would otherwise fail
                // outright.
                const auto existing = db.query_one(
                    "SELECT id, content_type FROM custom_fonts"
                    " WHERE owner_id = ? AND family_name = ?",
                    {DbValue(user_id), DbValue(family_name)});
                if (existing)
                {
                    const std::string old_id = existing->text(0);
                    std::error_code ec;
                    fs::remove(
                        font_file_path(user_id,
                                       old_id,
                                       ext_from_content_type(
                                           existing->text(1))),
                        ec);
                    db.execute("DELETE FROM custom_fonts WHERE id = ?",
                               {DbValue(old_id)});
                }

                db.execute(
                    "INSERT INTO custom_fonts (id, owner_id, family_name,"
                    " filename, content_type, byte_size, created_at)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?)",
                    {DbValue(id),
                     DbValue(user_id),
                     DbValue(family_name),
                     DbValue(filename),
                     DbValue(*content_type),
                     DbValue(static_cast<int64_t>(body.size())),
                     DbValue(util::now_ms())});

                return http::json_response(
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
            http::guard(std::move(callback), [&]() -> drogon::HttpResponsePtr {
                const std::string user_id =
                    auth::current_user_id_with_headers(req);
                validate_font_id(font_id);
                const FontRow row = load_font_row(font_id);
                if (row.owner_id != user_id)
                {
                    throw AppError::not_found();
                }
                const std::string ext = ext_from_content_type(row.content_type);

                if (req->method() == drogon::Delete)
                {
                    std::error_code ec;
                    fs::remove(font_file_path(user_id, font_id, ext), ec);
                    state().db->execute(
                        "DELETE FROM custom_fonts WHERE id = ?",
                        {DbValue(font_id)});
                    return http::ok_response();
                }

                const auto bytes =
                    util::read_file(font_file_path(user_id, font_id, ext));
                if (!bytes)
                {
                    throw AppError::not_found();
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
