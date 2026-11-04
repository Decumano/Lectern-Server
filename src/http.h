// Response construction and the try/catch shim that turns a thrown AppError
// into the JSON error body axum produced in the Rust original.
#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <functional>
#include <string>

#include "error.h"

namespace lectern {

/// Drogon hands every async handler this to deliver its response.
using HttpCallback = std::function<void(const drogon::HttpResponsePtr &)>;

}  // namespace lectern

namespace lectern::http {

inline drogon::HttpResponsePtr fJsonResponse(
    const nlohmann::json &body,
    drogon::HttpStatusCode status = drogon::k200OK)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

/// Plain-text body. `read_work_file` and friends return raw document text
/// rather than JSON, matching the frontend's `res.text()` calls.
inline drogon::HttpResponsePtr fTextResponse(std::string body)
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM,
                                            "text/plain; charset=utf-8");
    resp->setBody(std::move(body));
    return resp;
}

/// Bare 200 with no body, for endpoints that only signal success.
inline drogon::HttpResponsePtr fOkResponse()
{
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    return resp;
}

inline drogon::HttpResponsePtr fErrorResponse(const AppError &error)
{
    return fJsonResponse(nlohmann::json{{"error", error.fMessage()}},
                         error.fStatus());
}

/// Wraps a handler body. Anything that throws AppError becomes that error's
/// status and message; anything else becomes a 500 whose detail is logged but
/// not leaked to the client.
template <typename Callback, typename Fn>
void fGuard(Callback &&callback, Fn &&fn)
{
    try
    {
        callback(fn());
    }
    catch (const AppError &error)
    {
        callback(fErrorResponse(error));
    }
    catch (const std::exception &error)
    {
        spdlog::error("unhandled exception in handler: {}", error.what());
        callback(fErrorResponse(AppError::fInternal("internal server error")));
    }
}

/// Query parameter, or an empty string when absent — the Rust handlers used
/// `#[serde(default)]` for exactly this.
inline std::string fQuery(const drogon::HttpRequestPtr &req,
                         const std::string &name)
{
    return req->getParameter(name);
}

/// Query parameter that must be present and non-empty.
inline std::string fRequiredQuery(const drogon::HttpRequestPtr &req,
                                  const std::string &name)
{
    std::string value = req->getParameter(name);
    if (value.empty())
    {
        throw AppError::fBadRequest("missing '" + name + "' parameter");
    }
    return value;
}

inline bool fQueryBool(const drogon::HttpRequestPtr &req,
                       const std::string &name)
{
    const std::string value = req->getParameter(name);
    return value == "1" || value == "true";
}

/// Parses a JSON request body, rejecting anything malformed with a 400 rather
/// than letting the exception escape as a 500.
inline nlohmann::json fJsonBody(const drogon::HttpRequestPtr &req)
{
    const auto body = req->getBody();
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
    {
        throw AppError::fBadRequest("expected a JSON object body");
    }
    return parsed;
}

/// Required string field of a JSON object body.
inline std::string fJsonString(const nlohmann::json &object,
                               const char *key)
{
    if (!object.contains(key) || !object[key].is_string())
    {
        throw AppError::fBadRequest(std::string("missing '") + key +
                                    "' field");
    }
    return object[key].get<std::string>();
}

/// Optional string field; `fallback` when absent or not a string.
inline std::string fJsonStringOr(const nlohmann::json &object,
                                  const char *key,
                                  std::string fallback = {})
{
    if (!object.contains(key) || !object[key].is_string())
    {
        return fallback;
    }
    return object[key].get<std::string>();
}

inline bool fJsonBool(const nlohmann::json &object,
                      const char *key,
                      bool fallback = false)
{
    if (!object.contains(key) || !object[key].is_boolean())
    {
        return fallback;
    }
    return object[key].get<bool>();
}

}  // namespace lectern::http
