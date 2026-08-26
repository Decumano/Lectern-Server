#include "db.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

#include "error.h"
#include "util.h"

namespace fs = std::filesystem;

namespace lectern {

namespace {

/// DATABASE_URL in the Rust deployment is `sqlite://data/officesuite.db`;
/// the C API wants a bare path.
std::string path_from_url(const std::string &database_url)
{
    static constexpr std::string_view kPrefix = "sqlite://";
    if (util::starts_with(database_url, kPrefix))
    {
        std::string rest = database_url.substr(kPrefix.size());
        // `sqlite://data/x.db` and `sqlite:///abs/x.db` both appear in the
        // wild; a third slash means an absolute path, so keep it.
        return rest;
    }
    if (util::starts_with(database_url, "sqlite:"))
    {
        return database_url.substr(std::string_view("sqlite:").size());
    }
    return database_url;
}

class Statement
{
  public:
    Statement(sqlite3 *db, std::string_view sql)
    {
        const int rc = sqlite3_prepare_v2(db,
                                          sql.data(),
                                          static_cast<int>(sql.size()),
                                          &stmt_,
                                          nullptr);
        if (rc != SQLITE_OK || stmt_ == nullptr)
        {
            throw AppError::internal(std::string("sqlite prepare failed: ") +
                                     sqlite3_errmsg(db));
        }
    }

    ~Statement()
    {
        if (stmt_ != nullptr)
        {
            sqlite3_finalize(stmt_);
        }
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void bind(const DbParams &params)
    {
        int index = 1;
        for (const auto &param : params)
        {
            std::visit(
                [&](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                    {
                        sqlite3_bind_null(stmt_, index);
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        sqlite3_bind_int64(stmt_, index, value);
                    }
                    else if constexpr (std::is_same_v<T, bool>)
                    {
                        sqlite3_bind_int(stmt_, index, value ? 1 : 0);
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        sqlite3_bind_double(stmt_, index, value);
                    }
                    else
                    {
                        // SQLITE_TRANSIENT: sqlite copies the bytes, so the
                        // caller's string can die before the step().
                        sqlite3_bind_text(stmt_,
                                          index,
                                          value.data(),
                                          static_cast<int>(value.size()),
                                          SQLITE_TRANSIENT);
                    }
                },
                param);
            ++index;
        }
    }

    sqlite3_stmt *get()
    {
        return stmt_;
    }

  private:
    sqlite3_stmt *stmt_ = nullptr;
};

Row row_from(sqlite3_stmt *stmt)
{
    const int count = sqlite3_column_count(stmt);
    std::vector<std::optional<std::string>> columns;
    columns.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        if (sqlite3_column_type(stmt, i) == SQLITE_NULL)
        {
            columns.emplace_back(std::nullopt);
            continue;
        }
        const auto *text =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));
        const int bytes = sqlite3_column_bytes(stmt, i);
        columns.emplace_back(std::string(text == nullptr ? "" : text,
                                         static_cast<size_t>(bytes)));
    }
    return Row(std::move(columns));
}

}  // namespace

bool Row::is_null(size_t index) const
{
    return index >= columns_.size() || !columns_[index].has_value();
}

std::string Row::text(size_t index) const
{
    if (is_null(index))
    {
        return {};
    }
    return *columns_[index];
}

std::optional<std::string> Row::opt_text(size_t index) const
{
    if (is_null(index))
    {
        return std::nullopt;
    }
    return columns_[index];
}

int64_t Row::i64(size_t index) const
{
    if (is_null(index))
    {
        return 0;
    }
    try
    {
        return std::stoll(*columns_[index]);
    }
    catch (const std::exception &)
    {
        return 0;
    }
}

bool Row::boolean(size_t index) const
{
    return i64(index) != 0;
}

Db::Db(const std::string &database_url)
{
    const std::string path = path_from_url(database_url);

    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    const int rc = sqlite3_open_v2(
        path.c_str(),
        &handle_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK)
    {
        const std::string message =
            handle_ != nullptr ? sqlite3_errmsg(handle_) : "unknown error";
        if (handle_ != nullptr)
        {
            sqlite3_close(handle_);
            handle_ = nullptr;
        }
        throw std::runtime_error("cannot open database " + path + ": " +
                                 message);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // WAL keeps readers from blocking on the writer, which matters because
    // every request holds the connection mutex for the whole query.
    exec_raw("PRAGMA journal_mode = WAL");
    exec_raw("PRAGMA synchronous = NORMAL");
    // The schema declares REFERENCES constraints; SQLite ignores them unless
    // this is on, and the Rust deployment relied on sqlx enabling it.
    exec_raw("PRAGMA foreign_keys = ON");
    exec_raw("PRAGMA busy_timeout = 5000");
}

Db::~Db()
{
    if (handle_ != nullptr)
    {
        sqlite3_close(handle_);
    }
}

void Db::exec_raw(const std::string &sql)
{
    char *error = nullptr;
    const int rc =
        sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK)
    {
        const std::string message = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw AppError::internal("sqlite: " + message);
    }
}

int64_t Db::execute(std::string_view sql, const DbParams &params)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(handle_, sql);
    stmt.bind(params);

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
    {
        throw AppError::internal(std::string("sqlite step failed: ") +
                                 sqlite3_errmsg(handle_));
    }
    return sqlite3_changes64(handle_);
}

std::optional<Row> Db::query_one(std::string_view sql, const DbParams &params)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(handle_, sql);
    stmt.bind(params);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW)
    {
        return row_from(stmt.get());
    }
    if (rc == SQLITE_DONE)
    {
        return std::nullopt;
    }
    throw AppError::internal(std::string("sqlite step failed: ") +
                             sqlite3_errmsg(handle_));
}

std::vector<Row> Db::query_all(std::string_view sql, const DbParams &params)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(handle_, sql);
    stmt.bind(params);

    std::vector<Row> rows;
    while (true)
    {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW)
        {
            rows.push_back(row_from(stmt.get()));
            continue;
        }
        if (rc == SQLITE_DONE)
        {
            break;
        }
        throw AppError::internal(std::string("sqlite step failed: ") +
                                 sqlite3_errmsg(handle_));
    }
    return rows;
}

void Db::run_migrations(const fs::path &dir)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exec_raw(
            "CREATE TABLE IF NOT EXISTS _migrations ("
            "  name TEXT PRIMARY KEY,"
            "  applied_at INTEGER NOT NULL"
            ")");
    }

    if (!fs::is_directory(dir))
    {
        throw std::runtime_error("migrations directory not found: " +
                                 dir.string());
    }

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".sql")
        {
            files.push_back(entry.path());
        }
    }
    // Filename order is version order (0001_, 0002_, …), same as sqlx.
    std::sort(files.begin(), files.end());

    for (const auto &file : files)
    {
        const std::string name = file.filename().string();

        const auto applied =
            query_one("SELECT 1 FROM _migrations WHERE name = ?",
                      {DbValue(name)});
        if (applied.has_value())
        {
            continue;
        }

        const auto sql = util::read_file(file);
        if (!sql)
        {
            throw std::runtime_error("cannot read migration " + name);
        }

        spdlog::info("applying migration {}", name);

        std::lock_guard<std::mutex> lock(mutex_);
        exec_raw("BEGIN");
        try
        {
            // Migration files hold several statements; sqlite3_exec runs the
            // whole script, which prepared statements cannot.
            exec_raw(*sql);
            Statement stmt(
                handle_,
                "INSERT INTO _migrations (name, applied_at) VALUES (?, ?)");
            stmt.bind({DbValue(name), DbValue(util::now_s())});
            if (sqlite3_step(stmt.get()) != SQLITE_DONE)
            {
                throw AppError::internal(
                    std::string("recording migration failed: ") +
                    sqlite3_errmsg(handle_));
            }
            exec_raw("COMMIT");
        }
        catch (...)
        {
            char *ignored = nullptr;
            sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, &ignored);
            sqlite3_free(ignored);
            throw;
        }
    }
}

}  // namespace lectern
