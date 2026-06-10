#pragma once

#include "mini_oss/http.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mini_oss {

struct ObjectInfo {
    std::string id;
    std::string filename;
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::string sha256;
    std::string created_at;
};

class ObjectStore {
public:
    explicit ObjectStore(std::filesystem::path root_dir);

    HttpResponse createObject(const HttpRequest& request);
    HttpResponse listObjects(const HttpRequest& request);
    HttpResponse getObject(const HttpRequest& request);
    HttpResponse deleteObject(const HttpRequest& request);

private:
    static std::string extractObjectId(const std::string& path);
    static std::string sanitizeFilename(const std::string& filename);
    static std::string sha256Hex(const std::string& data);
    static std::string jsonEscape(const std::string& value);
    static std::string now();

    std::filesystem::path root_dir_;
    std::filesystem::path object_dir_;
    std::mutex mutex_;
    std::unordered_map<std::string, ObjectInfo> objects_;
    std::uint64_t next_id_ = 1;
};

} // namespace mini_oss
