#include "mini_oss/metadata_store.h"

#include <sqlite3.h>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace mini_oss {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const char* sql, std::string& error)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            error = sqlite3_errmsg(db);
        }
    }

    ~Statement()
    {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const
    {
        return stmt_;
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::string columnText(sqlite3_stmt* stmt, int index)
{
    const auto* text = sqlite3_column_text(stmt, index);
    return text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
}

bool parseId(const std::string& id, sqlite3_int64& value)
{
    if (id.empty()) {
        return false;
    }

    char* end = nullptr;
    const long long parsed = std::strtoll(id.c_str(), &end, 10);
    if (end == id.c_str() || *end != '\0' || parsed <= 0) {
        return false;
    }
    value = static_cast<sqlite3_int64>(parsed);
    return true;
}

ObjectInfo rowToObject(sqlite3_stmt* stmt)
{
    ObjectInfo info;
    info.id = std::to_string(sqlite3_column_int64(stmt, 0));
    info.filename = columnText(stmt, 1);
    info.path = columnText(stmt, 2);
    info.size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
    info.sha256 = columnText(stmt, 4);
    info.created_at = columnText(stmt, 5);
    return info;
}

} // namespace

MetadataStore::MetadataStore(std::filesystem::path db_path)
    : db_path_(std::move(db_path))
{
    initialize();
}

MetadataStore::~MetadataStore()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MetadataStore::ready() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ != nullptr;
}

std::string MetadataStore::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::optional<std::string> MetadataStore::nextObjectId(std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return std::nullopt;
    }

    Statement stmt(db_, "SELECT COALESCE(MAX(id), 0) + 1 FROM objects;", error);
    if (stmt.get() == nullptr) {
        return std::nullopt;
    }

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        error = sqlite3_errmsg(db_);
        return std::nullopt;
    }
    return std::to_string(sqlite3_column_int64(stmt.get(), 0));
}

bool MetadataStore::insertObject(const ObjectInfo& info, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return false;
    }

    Statement stmt(db_,
                   "INSERT INTO objects(id, filename, path, size, sha256, created_at) "
                   "VALUES(?, ?, ?, ?, ?, ?);",
                   error);
    if (stmt.get() == nullptr) {
        return false;
    }

    sqlite3_int64 id = 0;
    if (!parseId(info.id, id)) {
        error = "invalid object id";
        return false;
    }

    const std::string path = info.path.string();
    sqlite3_bind_int64(stmt.get(), 1, id);
    sqlite3_bind_text(stmt.get(), 2, info.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(info.size));
    sqlite3_bind_text(stmt.get(), 5, info.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, info.created_at.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

std::optional<ObjectInfo> MetadataStore::getObject(const std::string& id, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return std::nullopt;
    }

    Statement stmt(db_,
                   "SELECT id, filename, path, size, sha256, created_at "
                   "FROM objects WHERE id = ?;",
                   error);
    if (stmt.get() == nullptr) {
        return std::nullopt;
    }

    sqlite3_int64 parsed_id = 0;
    if (!parseId(id, parsed_id)) {
        error = "invalid object id";
        return std::nullopt;
    }
    sqlite3_bind_int64(stmt.get(), 1, parsed_id);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return rowToObject(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
    }
    return std::nullopt;
}

std::optional<ObjectInfo> MetadataStore::findObjectBySha256(const std::string& sha256,
                                                            std::uint64_t size,
                                                            std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return std::nullopt;
    }

    Statement stmt(db_,
                   "SELECT id, filename, path, size, sha256, created_at "
                   "FROM objects WHERE sha256 = ? AND size = ? ORDER BY id ASC LIMIT 1;",
                   error);
    if (stmt.get() == nullptr) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt.get(), 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(size));

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        return rowToObject(stmt.get());
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
    }
    return std::nullopt;
}

std::vector<ObjectInfo> MetadataStore::listObjects(std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ObjectInfo> objects;
    if (db_ == nullptr) {
        error = last_error_;
        return objects;
    }

    Statement stmt(db_,
                   "SELECT id, filename, path, size, sha256, created_at "
                   "FROM objects ORDER BY id DESC;",
                   error);
    if (stmt.get() == nullptr) {
        return objects;
    }

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            objects.push_back(rowToObject(stmt.get()));
            continue;
        }
        if (rc == SQLITE_DONE) {
            break;
        }
        error = sqlite3_errmsg(db_);
        break;
    }
    return objects;
}

bool MetadataStore::deleteObject(const std::string& id, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return false;
    }

    Statement stmt(db_, "DELETE FROM objects WHERE id = ?;", error);
    if (stmt.get() == nullptr) {
        return false;
    }

    sqlite3_int64 parsed_id = 0;
    if (!parseId(id, parsed_id)) {
        error = "invalid object id";
        return false;
    }
    sqlite3_bind_int64(stmt.get(), 1, parsed_id);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

std::uint64_t MetadataStore::countObjectsByPath(const std::filesystem::path& path,
                                                std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return 0;
    }

    Statement stmt(db_, "SELECT COUNT(*) FROM objects WHERE path = ?;", error);
    if (stmt.get() == nullptr) {
        return 0;
    }

    const std::string path_text = path.string();
    sqlite3_bind_text(stmt.get(), 1, path_text.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        error = sqlite3_errmsg(db_);
        return 0;
    }
    return static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0));
}

bool MetadataStore::initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    if (ec) {
        last_error_ = "cannot create metadata directory: " + ec.message();
        return false;
    }

    if (sqlite3_open(db_path_.string().c_str(), &db_) != SQLITE_OK) {
        last_error_ = db_ == nullptr ? "cannot open sqlite database" : sqlite3_errmsg(db_);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    if (!exec("PRAGMA journal_mode=WAL;")) {
        return false;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS objects ("
              "id INTEGER PRIMARY KEY,"
              "filename TEXT NOT NULL,"
              "path TEXT NOT NULL,"
              "size INTEGER NOT NULL,"
              "sha256 TEXT NOT NULL,"
              "created_at TEXT NOT NULL"
              ");")) {
        return false;
    }
    if (!exec("CREATE INDEX IF NOT EXISTS idx_objects_sha256_size "
              "ON objects(sha256, size);")) {
        return false;
    }
    if (!exec("CREATE INDEX IF NOT EXISTS idx_objects_path "
              "ON objects(path);")) {
        return false;
    }
    return true;
}

bool MetadataStore::exec(const char* sql)
{
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        last_error_ = errmsg == nullptr ? sqlite3_errmsg(db_) : errmsg;
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

} // namespace mini_oss
