#pragma once

#include "mini_oss/http.h"
#include "mini_oss/metadata_store.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>

namespace mini_oss {

class ObjectStore {
public:
    explicit ObjectStore(std::filesystem::path root_dir);

    HttpResponse createObject(const HttpRequest& request);
    HttpResponse createInstantObject(const HttpRequest& request);
    HttpResponse listObjects(const HttpRequest& request);
    HttpResponse getObject(const HttpRequest& request);
    HttpResponse deleteObject(const HttpRequest& request);

private:
    static std::string extractObjectId(const std::string& path);
    static std::string sanitizeFilename(const std::string& filename);
    static bool isValidSha256(const std::string& sha256);
    static std::optional<std::uint64_t> parseSize(const std::string& value);
    static std::string sha256Hex(const std::string& data);
    static std::string now();
    HttpResponse createMetadataAlias(const ObjectInfo& source, const std::string& filename,
                                     bool instant_upload);

    std::filesystem::path root_dir_;
    std::filesystem::path object_dir_;
    MetadataStore metadata_store_;
    mutable std::shared_mutex mutex_;
};

} // namespace mini_oss
