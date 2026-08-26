// Thin synchronous wrapper over the SQLite C API.
//
// The Rust server used sqlx with an async pool. Drogon can do async DB work,
// but only when built with its `orm`+`sqlite3` features, and every handler in
// this application already does blocking filesystem I/O (reading and writing
// workspace files) on the same thread — so an async DB layer would buy nothing
// while adding a second concurrency model. Instead the whole connection is
// serialised behind one mutex and drogon is given a thread pool to absorb the
// blocking (see main.cpp's setThreadNum).
//
// SQLite is a local file; a query here is microseconds, not a network round
// trip. If that ever stops being true, this is the seam to make async.
#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lectern {

/// A bound parameter. `std::monostate` binds SQL NULL.
using DbValue =
    std::variant<std::monostate, int64_t, double, std::string, bool>;
using DbParams = std::vector<DbValue>;

/// One result row, materialised as text (SQLite is dynamically typed and
/// every column this schema uses is TEXT or INTEGER).
class Row
{
  public:
    explicit Row(std::vector<std::optional<std::string>> columns)
        : columns_(std::move(columns))
    {
    }

    bool is_null(size_t index) const;

    /// Column as text; empty string when NULL or out of range.
    std::string text(size_t index) const;

    /// Column as text, or nullopt when SQL NULL — for the genuinely nullable
    /// columns (comments.author_id, comments.anchor).
    std::optional<std::string> opt_text(size_t index) const;

    int64_t i64(size_t index) const;

    /// SQLite has no boolean type; the schema stores 0/1 integers.
    bool boolean(size_t index) const;

    size_t size() const
    {
        return columns_.size();
    }

  private:
    std::vector<std::optional<std::string>> columns_;
};

class Db
{
  public:
    /// Opens (creating if missing) the database file. Accepts either a plain
    /// path or a `sqlite://…` URL, so DATABASE_URL from the Rust deployment
    /// keeps working unchanged.
    explicit Db(const std::string &database_url);
    ~Db();

    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    /// Runs a statement, returning the number of rows it changed.
    int64_t execute(std::string_view sql, const DbParams &params = {});

    std::optional<Row> query_one(std::string_view sql,
                                 const DbParams &params = {});

    std::vector<Row> query_all(std::string_view sql,
                               const DbParams &params = {});

    /// Applies every `*.sql` file in `dir` that this database has not seen
    /// yet, in filename order, each inside its own transaction. Replaces
    /// sqlx's compile-time `migrate!` macro; the ledger table is `_migrations`.
    void run_migrations(const std::filesystem::path &dir);

  private:
    sqlite3 *handle_ = nullptr;
    std::mutex mutex_;

    /// Caller must hold `mutex_`.
    void exec_raw(const std::string &sql);
};

}  // namespace lectern
