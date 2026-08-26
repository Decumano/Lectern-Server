// The Rust original returned `Result<T, AppError>` and let axum turn the error
// into a JSON body. C++ has no equivalent of `?`, so AppError is an exception:
// handlers throw it and `respond()` in http.h turns it into the same
// `{"error": "..."}` payload with the same status code.
#pragma once

#include <drogon/HttpTypes.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace lectern {

class AppError : public std::runtime_error
{
  public:
    AppError(drogon::HttpStatusCode status, std::string message)
        : std::runtime_error(message),
          status_(status),
          message_(std::move(message))
    {
    }

    drogon::HttpStatusCode status() const noexcept
    {
        return status_;
    }

    const std::string &message() const noexcept
    {
        return message_;
    }

    static AppError bad_request(std::string message)
    {
        return {drogon::k400BadRequest, std::move(message)};
    }

    static AppError unauthorized()
    {
        return {drogon::k401Unauthorized, "unauthorized"};
    }

    static AppError forbidden()
    {
        return {drogon::k403Forbidden,
                "you don't have permission to do that"};
    }

    static AppError not_found()
    {
        return {drogon::k404NotFound, "not found"};
    }

    static AppError conflict(std::string message)
    {
        return {drogon::k409Conflict, std::move(message)};
    }

    static AppError too_many_requests()
    {
        return {drogon::k429TooManyRequests,
                "too many attempts, try again later"};
    }

    static AppError internal(std::string message)
    {
        return {drogon::k500InternalServerError, std::move(message)};
    }

  private:
    drogon::HttpStatusCode status_;
    std::string message_;
};

}  // namespace lectern
