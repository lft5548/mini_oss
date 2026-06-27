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
    info.owner_user_id = sqlite3_column_int(stmt, 6);
    info.upload_count = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
    info.download_count = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
    return info;
}

UserRecord rowToUser(sqlite3_stmt* stmt)
{
    UserRecord user;
    user.id = sqlite3_column_int(stmt, 0);
    user.username = columnText(stmt, 1);
    user.password_hash = columnText(stmt, 2);
    user.password_salt = columnText(stmt, 3);
    user.status = sqlite3_column_int(stmt, 4);
    user.created_at = columnText(stmt, 5);
    user.updated_at = columnText(stmt, 6);
    user.last_login_at = columnText(stmt, 7);
    return user;
}

std::vector<std::string> loadRoles(sqlite3* db, int user_id, std::string& error)
{
    std::vector<std::string> roles;
    Statement stmt(db,
                   "SELECT r.role_name FROM roles r "
                   "JOIN user_roles ur ON ur.role_id = r.id "
                   "WHERE ur.user_id = ? ORDER BY r.role_name ASC;",
                   error);
    if (stmt.get() == nullptr) {
        return roles;
    }
    sqlite3_bind_int(stmt.get(), 1, user_id);

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            roles.push_back(columnText(stmt.get(), 0));
            continue;
        }
        if (rc == SQLITE_DONE) {
            break;
        }
        error = sqlite3_errmsg(db);
        break;
    }
    return roles;
}

AuditLogRecord rowToAuditLog(sqlite3_stmt* stmt)
{
    AuditLogRecord record;
    record.id = sqlite3_column_int64(stmt, 0);
    record.request_id = columnText(stmt, 1);
    record.user_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 2);
    record.username = columnText(stmt, 3);
    record.method = columnText(stmt, 4);
    record.path = columnText(stmt, 5);
    record.action = columnText(stmt, 6);
    record.file_id = columnText(stmt, 7);
    record.status_code = sqlite3_column_int(stmt, 8);
    record.result = columnText(stmt, 9);
    record.error_message = columnText(stmt, 10);
    record.client_ip = columnText(stmt, 11);
    record.latency_ms = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 12));
    record.created_at = columnText(stmt, 13);
    return record;
}

constexpr const char* kObjectColumns =
    "id, filename, path, size, sha256, created_at, owner_user_id, upload_count, download_count";

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
                   "INSERT INTO objects(id, filename, path, size, sha256, created_at, "
                   "owner_user_id, upload_count, download_count) "
                   "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?);",
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
    sqlite3_bind_int(stmt.get(), 7, info.owner_user_id);
    sqlite3_bind_int64(stmt.get(), 8, static_cast<sqlite3_int64>(info.upload_count));
    sqlite3_bind_int64(stmt.get(), 9, static_cast<sqlite3_int64>(info.download_count));

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

    const std::string sql = std::string("SELECT ") + kObjectColumns + " FROM objects WHERE id = ?;";
    Statement stmt(db_, sql.c_str(), error);
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

    const std::string sql = std::string("SELECT ") + kObjectColumns
        + " FROM objects WHERE sha256 = ? AND size = ? ORDER BY id ASC LIMIT 1;";
    Statement stmt(db_, sql.c_str(), error);
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

    const std::string sql = std::string("SELECT ") + kObjectColumns + " FROM objects ORDER BY id DESC;";
    Statement stmt(db_, sql.c_str(), error);
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

std::vector<ObjectInfo> MetadataStore::listObjectsForOwner(int owner_user_id, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ObjectInfo> objects;
    if (db_ == nullptr) {
        error = last_error_;
        return objects;
    }

    const std::string sql = std::string("SELECT ") + kObjectColumns
        + " FROM objects WHERE owner_user_id = ? ORDER BY id DESC;";
    Statement stmt(db_, sql.c_str(), error);
    if (stmt.get() == nullptr) {
        return objects;
    }
    sqlite3_bind_int(stmt.get(), 1, owner_user_id);

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

bool MetadataStore::incrementObjectDownloadCount(const std::string& id, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ == nullptr) {
        error = last_error_;
        return false;
    }

    Statement stmt(db_, "UPDATE objects SET download_count = download_count + 1 WHERE id = ?;", error);
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

ObjectStorageStats MetadataStore::objectStorageStats(std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ObjectStorageStats stats;
    if (db_ == nullptr) {
        error = last_error_;
        return stats;
    }

    Statement counts(db_,
                     "SELECT COUNT(*), COALESCE(SUM(upload_count), 0), "
                     "COALESCE(SUM(download_count), 0) FROM objects;",
                     error);
    if (counts.get() == nullptr) {
        return stats;
    }
    if (sqlite3_step(counts.get()) != SQLITE_ROW) {
        error = sqlite3_errmsg(db_);
        return stats;
    }
    stats.file_total = static_cast<std::uint64_t>(sqlite3_column_int64(counts.get(), 0));
    stats.upload_count = static_cast<std::uint64_t>(sqlite3_column_int64(counts.get(), 1));
    stats.download_count = static_cast<std::uint64_t>(sqlite3_column_int64(counts.get(), 2));

    Statement storage(db_,
                      "SELECT COALESCE(SUM(size), 0) FROM "
                      "(SELECT path, MAX(size) AS size FROM objects GROUP BY path);",
                      error);
    if (storage.get() == nullptr) {
        return stats;
    }
    if (sqlite3_step(storage.get()) != SQLITE_ROW) {
        error = sqlite3_errmsg(db_);
        return stats;
    }
    stats.storage_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(storage.get(), 0));
    return stats;
}

bool MetadataStore::ensureRole(const std::string& role_name, const std::string& description,
                               std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT OR IGNORE INTO roles(role_name, description, created_at) "
                   "VALUES(?, ?, strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
                   error);
    if (stmt.get() == nullptr) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, role_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, description.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool MetadataStore::ensureUser(const std::string& username, const std::string& password_hash,
                               const std::string& password_salt, const std::string& now,
                               std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT OR IGNORE INTO users(username, password_hash, password_salt, status, "
                   "created_at, updated_at) VALUES(?, ?, ?, 1, ?, ?);",
                   error);
    if (stmt.get() == nullptr) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, password_salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, now.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool MetadataStore::assignRole(const std::string& username, const std::string& role_name,
                               std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT OR IGNORE INTO user_roles(user_id, role_id, created_at) "
                   "SELECT u.id, r.id, strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                   "FROM users u JOIN roles r WHERE u.username = ? AND r.role_name = ?;",
                   error);
    if (stmt.get() == nullptr) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, role_name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

std::optional<UserRecord> MetadataStore::findUserByUsername(const std::string& username,
                                                            std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "SELECT id, username, password_hash, password_salt, status, created_at, "
                   "updated_at, COALESCE(last_login_at, '') FROM users WHERE username = ?;",
                   error);
    if (stmt.get() == nullptr) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt.get(), 1, username.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        auto user = rowToUser(stmt.get());
        user.roles = loadRoles(db_, user.id, error);
        return user;
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
    }
    return std::nullopt;
}

std::optional<UserRecord> MetadataStore::findUserByToken(const std::string& token,
                                                         std::int64_t now_epoch_seconds,
                                                         std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "SELECT u.id, u.username, u.password_hash, u.password_salt, u.status, "
                   "u.created_at, u.updated_at, COALESCE(u.last_login_at, '') "
                   "FROM auth_sessions s JOIN users u ON u.id = s.user_id "
                   "WHERE s.token = ? AND s.expires_at > ?;",
                   error);
    if (stmt.get() == nullptr) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt.get(), 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 2, now_epoch_seconds);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        auto user = rowToUser(stmt.get());
        user.roles = loadRoles(db_, user.id, error);
        return user;
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
    }
    return std::nullopt;
}

bool MetadataStore::createSession(const std::string& token, int user_id, std::int64_t created_at,
                                  std::int64_t expires_at, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT OR REPLACE INTO auth_sessions(token, user_id, created_at, expires_at) "
                   "VALUES(?, ?, ?, ?);",
                   error);
    if (stmt.get() == nullptr) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, user_id);
    sqlite3_bind_int64(stmt.get(), 3, created_at);
    sqlite3_bind_int64(stmt.get(), 4, expires_at);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool MetadataStore::updateLastLogin(int user_id, const std::string& now, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "UPDATE users SET last_login_at = ?, updated_at = ? WHERE id = ?;", error);
    if (stmt.get() == nullptr) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 3, user_id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

std::vector<UserRecord> MetadataStore::listUsers(std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UserRecord> users;
    Statement stmt(db_,
                   "SELECT id, username, password_hash, password_salt, status, created_at, "
                   "updated_at, COALESCE(last_login_at, '') FROM users ORDER BY id ASC;",
                   error);
    if (stmt.get() == nullptr) {
        return users;
    }

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            auto user = rowToUser(stmt.get());
            user.roles = loadRoles(db_, user.id, error);
            users.push_back(std::move(user));
            continue;
        }
        if (rc == SQLITE_DONE) {
            break;
        }
        error = sqlite3_errmsg(db_);
        break;
    }
    return users;
}

bool MetadataStore::replaceUserRoles(int user_id, const std::vector<std::string>& role_names,
                                     std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!exec("BEGIN IMMEDIATE TRANSACTION;")) {
        error = last_error_;
        return false;
    }

    Statement delete_stmt(db_, "DELETE FROM user_roles WHERE user_id = ?;", error);
    if (delete_stmt.get() == nullptr) {
        exec("ROLLBACK;");
        return false;
    }
    sqlite3_bind_int(delete_stmt.get(), 1, user_id);
    if (sqlite3_step(delete_stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        exec("ROLLBACK;");
        return false;
    }

    for (const auto& role_name : role_names) {
        Statement insert_stmt(db_,
                              "INSERT INTO user_roles(user_id, role_id, created_at) "
                              "SELECT ?, id, strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                              "FROM roles WHERE role_name = ?;",
                              error);
        if (insert_stmt.get() == nullptr) {
            exec("ROLLBACK;");
            return false;
        }
        sqlite3_bind_int(insert_stmt.get(), 1, user_id);
        sqlite3_bind_text(insert_stmt.get(), 2, role_name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(insert_stmt.get()) != SQLITE_DONE) {
            error = sqlite3_errmsg(db_);
            exec("ROLLBACK;");
            return false;
        }
    }

    if (!exec("COMMIT;")) {
        error = last_error_;
        exec("ROLLBACK;");
        return false;
    }
    return true;
}

bool MetadataStore::insertAuditLog(const AuditLogRecord& record, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_,
                   "INSERT INTO audit_logs(request_id, user_id, username, method, path, action, "
                   "file_id, status_code, result, error_message, client_ip, latency_ms, created_at) "
                   "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                   error);
    if (stmt.get() == nullptr) {
        return false;
    }
    sqlite3_bind_text(stmt.get(), 1, record.request_id.c_str(), -1, SQLITE_TRANSIENT);
    if (record.user_id > 0) {
        sqlite3_bind_int(stmt.get(), 2, record.user_id);
    } else {
        sqlite3_bind_null(stmt.get(), 2);
    }
    sqlite3_bind_text(stmt.get(), 3, record.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, record.method.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, record.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, record.action.c_str(), -1, SQLITE_TRANSIENT);
    if (record.file_id.empty()) {
        sqlite3_bind_null(stmt.get(), 7);
    } else {
        sqlite3_bind_text(stmt.get(), 7, record.file_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt.get(), 8, record.status_code);
    sqlite3_bind_text(stmt.get(), 9, record.result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 10, record.error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 11, record.client_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 12, static_cast<sqlite3_int64>(record.latency_ms));
    sqlite3_bind_text(stmt.get(), 13, record.created_at.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

std::vector<AuditLogRecord> MetadataStore::listAuditLogs(const AuditLogQuery& query,
                                                         std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuditLogRecord> records;
    std::string sql =
        "SELECT id, request_id, user_id, username, method, path, action, file_id, status_code, "
        "result, error_message, client_ip, latency_ms, created_at FROM audit_logs WHERE 1 = 1";
    if (query.user_id > 0) {
        sql += " AND user_id = ?";
    }
    if (!query.action.empty()) {
        sql += " AND action = ?";
    }
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?;";

    Statement stmt(db_, sql.c_str(), error);
    if (stmt.get() == nullptr) {
        return records;
    }

    int bind_index = 1;
    if (query.user_id > 0) {
        sqlite3_bind_int(stmt.get(), bind_index++, query.user_id);
    }
    if (!query.action.empty()) {
        sqlite3_bind_text(stmt.get(), bind_index++, query.action.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(query.limit));
    sqlite3_bind_int64(stmt.get(), bind_index++, static_cast<sqlite3_int64>(query.offset));

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            records.push_back(rowToAuditLog(stmt.get()));
            continue;
        }
        if (rc == SQLITE_DONE) {
            break;
        }
        error = sqlite3_errmsg(db_);
        break;
    }
    return records;
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
    if (!addColumnIfMissingLocked("objects", "owner_user_id",
                                  "owner_user_id INTEGER NOT NULL DEFAULT 0")) {
        return false;
    }
    if (!addColumnIfMissingLocked("objects", "upload_count",
                                  "upload_count INTEGER NOT NULL DEFAULT 1")) {
        return false;
    }
    if (!addColumnIfMissingLocked("objects", "download_count",
                                  "download_count INTEGER NOT NULL DEFAULT 0")) {
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
    if (!exec("CREATE INDEX IF NOT EXISTS idx_objects_owner "
              "ON objects(owner_user_id);")) {
        return false;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS users ("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "username TEXT NOT NULL UNIQUE,"
              "password_hash TEXT NOT NULL,"
              "password_salt TEXT NOT NULL,"
              "status INTEGER NOT NULL DEFAULT 1,"
              "created_at TEXT NOT NULL,"
              "updated_at TEXT NOT NULL,"
              "last_login_at TEXT"
              ");")) {
        return false;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS roles ("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "role_name TEXT NOT NULL UNIQUE,"
              "description TEXT,"
              "created_at TEXT NOT NULL"
              ");")) {
        return false;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS user_roles ("
              "user_id INTEGER NOT NULL,"
              "role_id INTEGER NOT NULL,"
              "created_at TEXT NOT NULL,"
              "PRIMARY KEY(user_id, role_id),"
              "FOREIGN KEY(user_id) REFERENCES users(id),"
              "FOREIGN KEY(role_id) REFERENCES roles(id)"
              ");")) {
        return false;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS auth_sessions ("
              "token TEXT PRIMARY KEY,"
              "user_id INTEGER NOT NULL,"
              "created_at INTEGER NOT NULL,"
              "expires_at INTEGER NOT NULL,"
              "FOREIGN KEY(user_id) REFERENCES users(id)"
              ");")) {
        return false;
    }
    if (!exec("CREATE INDEX IF NOT EXISTS idx_auth_sessions_user "
              "ON auth_sessions(user_id);")) {
        return false;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS audit_logs ("
              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "request_id TEXT NOT NULL,"
              "user_id INTEGER,"
              "username TEXT,"
              "method TEXT NOT NULL,"
              "path TEXT NOT NULL,"
              "action TEXT NOT NULL,"
              "file_id TEXT,"
              "status_code INTEGER NOT NULL,"
              "result TEXT NOT NULL,"
              "error_message TEXT,"
              "client_ip TEXT,"
              "latency_ms INTEGER NOT NULL,"
              "created_at TEXT NOT NULL"
              ");")) {
        return false;
    }
    if (!exec("CREATE INDEX IF NOT EXISTS idx_audit_user_time "
              "ON audit_logs(user_id, created_at);")) {
        return false;
    }
    if (!exec("CREATE INDEX IF NOT EXISTS idx_audit_action_time "
              "ON audit_logs(action, created_at);")) {
        return false;
    }
    if (!exec("CREATE INDEX IF NOT EXISTS idx_audit_file_id "
              "ON audit_logs(file_id);")) {
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

bool MetadataStore::columnExistsLocked(const std::string& table, const std::string& column,
                                       std::string& error)
{
    const std::string sql = "PRAGMA table_info(" + table + ");";
    Statement stmt(db_, sql.c_str(), error);
    if (stmt.get() == nullptr) {
        return false;
    }

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_ROW) {
            if (columnText(stmt.get(), 1) == column) {
                return true;
            }
            continue;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        error = sqlite3_errmsg(db_);
        return false;
    }
}

bool MetadataStore::addColumnIfMissingLocked(const std::string& table, const std::string& column,
                                             const std::string& definition)
{
    std::string error;
    if (columnExistsLocked(table, column, error)) {
        return true;
    }
    if (!error.empty()) {
        last_error_ = error;
        return false;
    }

    const std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + definition + ";";
    return exec(sql.c_str());
}

} // namespace mini_oss
