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
    std::vector<ObjectInfo> listObjects(std::string& error);
    bool deleteObject(const std::string& id, std::string& error);

private:
    bool initialize();
    bool exec(const char* sql);

    mutable std::mutex mutex_;
    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
    std::string last_error_;
};

} // namespace mini_oss
