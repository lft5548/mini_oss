#pragma once

#include "mini_oss/config.h"
#include "mini_oss/http.h"
#include "mini_oss/metadata_store.h"
#include "mini_oss/metrics.h"
#include "mini_oss/redis_metadata_cache.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>

namespace mini_oss {

class ObjectStore {
public:
    explicit ObjectStore(std::filesystem::path root_dir, RedisConfig redis_config = {},
                         Metrics* metrics = nullptr);

    HttpResponse createObject(const HttpRequest& request);
    HttpResponse createInstantObject(const HttpRequest& request);
    HttpResponse listObjects(const HttpRequest& request);
    HttpResponse getObject(const HttpRequest& request);
    HttpResponse deleteObject(const HttpRequest& request);

    MetadataStore& metadataStore();
    const MetadataStore& metadataStore() const;

private:
    struct ByteRange {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
    };

    static std::string extractObjectId(const std::string& path);
    static std::string sanitizeFilename(const std::string& filename);
    static bool isValidSha256(const std::string& sha256);
    static std::optional<std::uint64_t> parseSize(const std::string& value);
    static std::optional<ByteRange> parseRangeHeader(const std::string& value, std::uint64_t total_size);
    static std::string sha256Hex(const std::string& data);
    static std::string now();
    static std::optional<std::string> sha256File(const std::filesystem::path& path,
                                                   std::uint64_t& file_size,
                                                   std::string& error);
    HttpResponse createObjectFromFileBody(const HttpRequest& request);
    HttpResponse createMetadataAlias(const ObjectInfo& source, const std::string& filename,
                                     bool instant_upload, int owner_user_id);
    std::optional<ObjectInfo> getObjectMetadata(const std::string& id, std::string& error);
    std::optional<ObjectInfo> findObjectBySha256Cached(const std::string& sha256,
                                                       std::uint64_t size,
                                                       std::string& error);
    bool insertObjectMetadata(const ObjectInfo& info, std::string& error);
    bool deleteObjectMetadata(const ObjectInfo& info, std::string& error);

    std::filesystem::path root_dir_;
    std::filesystem::path object_dir_;
    std::filesystem::path temp_upload_dir_;
    MetadataStore metadata_store_;
    RedisMetadataCache metadata_cache_;
    mutable std::shared_mutex mutex_;
};

} // namespace mini_oss
