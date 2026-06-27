#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace mini_oss {

struct ObjectInfo {
    std::string id;
    std::string filename;
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::string sha256;
    std::string created_at;
    int owner_user_id = 0;
    std::uint64_t upload_count = 1;
    std::uint64_t download_count = 0;
};

struct UserRecord {
    int id = 0;
    std::string username;
    std::string password_hash;
    std::string password_salt;
    int status = 1;
    std::string created_at;
    std::string updated_at;
    std::string last_login_at;
    std::vector<std::string> roles;
};

struct AuditLogRecord {
    std::int64_t id = 0;
    std::string request_id;
    int user_id = 0;
    std::string username;
    std::string method;
    std::string path;
    std::string action;
    std::string file_id;
    int status_code = 0;
    std::string result;
    std::string error_message;
    std::string client_ip;
    std::uint64_t latency_ms = 0;
    std::string created_at;
};

struct AuditLogQuery {
    int user_id = 0;
    std::string action;
    std::size_t limit = 100;
    std::size_t offset = 0;
};

struct ObjectStorageStats {
    std::uint64_t file_total = 0;
    std::uint64_t storage_bytes = 0;
    std::uint64_t upload_count = 0;
    std::uint64_t download_count = 0;
};

class MetadataStore {
public:
    explicit MetadataStore(std::filesystem::path db_path);
    ~MetadataStore();

    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;

    bool ready() const;
    std::string lastError() const;

    std::optional<std::string> nextObjectId(std::string& error);
    bool insertObject(const ObjectInfo& info, std::string& error);
    std::optional<ObjectInfo> getObject(const std::string& id, std::string& error);
    std::optional<ObjectInfo> findObjectBySha256(const std::string& sha256, std::uint64_t size,
                                                 std::string& error);
    std::vector<ObjectInfo> listObjects(std::string& error);
    std::vector<ObjectInfo> listObjectsForOwner(int owner_user_id, std::string& error);
    bool deleteObject(const std::string& id, std::string& error);
    bool incrementObjectDownloadCount(const std::string& id, std::string& error);
    std::uint64_t countObjectsByPath(const std::filesystem::path& path, std::string& error);
    ObjectStorageStats objectStorageStats(std::string& error);

    bool ensureRole(const std::string& role_name, const std::string& description,
                    std::string& error);
    bool ensureUser(const std::string& username, const std::string& password_hash,
                    const std::string& password_salt, const std::string& now,
                    std::string& error);
    bool assignRole(const std::string& username, const std::string& role_name,
                    std::string& error);
    std::optional<UserRecord> findUserByUsername(const std::string& username,
                                                 std::string& error);
    std::optional<UserRecord> findUserByToken(const std::string& token,
                                              std::int64_t now_epoch_seconds,
                                              std::string& error);
    bool createSession(const std::string& token, int user_id, std::int64_t created_at,
                       std::int64_t expires_at, std::string& error);
    bool updateLastLogin(int user_id, const std::string& now, std::string& error);
    std::vector<UserRecord> listUsers(std::string& error);
    bool replaceUserRoles(int user_id, const std::vector<std::string>& role_names,
                          std::string& error);

    bool insertAuditLog(const AuditLogRecord& record, std::string& error);
    std::vector<AuditLogRecord> listAuditLogs(const AuditLogQuery& query, std::string& error);

private:
    bool initialize();
    bool exec(const char* sql);
    bool columnExistsLocked(const std::string& table, const std::string& column,
                            std::string& error);
    bool addColumnIfMissingLocked(const std::string& table, const std::string& column,
                                  const std::string& definition);

    mutable std::mutex mutex_;
    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
    std::string last_error_;
};

} // namespace mini_oss
