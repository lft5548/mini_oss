#pragma once

#include "mini_oss/config.h"
#include "mini_oss/metadata_store.h"
#include "mini_oss/metrics.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct redisContext;

namespace mini_oss {

class RedisMetadataCache {
public:
    explicit RedisMetadataCache(RedisConfig config = {}, Metrics* metrics = nullptr);
    ~RedisMetadataCache();

    RedisMetadataCache(const RedisMetadataCache&) = delete;
    RedisMetadataCache& operator=(const RedisMetadataCache&) = delete;

    bool enabled() const;
    std::string lastError() const;

    std::optional<ObjectInfo> getObject(const std::string& id);
    std::optional<std::string> getShaIndex(const std::string& sha256, std::uint64_t size);
    void putObject(const ObjectInfo& info);
    void putShaIndex(const std::string& sha256, std::uint64_t size, const std::string& id);
    void deleteObject(const ObjectInfo& info);
    void deleteShaIndex(const std::string& sha256, std::uint64_t size);

private:
    bool ensureConnectedLocked();
    bool authenticateLocked();
    bool selectDbLocked();
    void closeLocked();
    void commandErrorLocked(const std::string& message);
    void postponeAfterInvalidationFailureLocked();
    std::optional<std::string> getStringLocked(const std::string& key);
    bool setStringLocked(const std::string& key, const std::string& value);
    void delKeyLocked(const std::string& key);
    std::string objectKey(const std::string& id) const;
    std::string shaKey(const std::string& sha256, std::uint64_t size) const;

    RedisConfig config_;
    Metrics* metrics_ = nullptr;
    mutable std::mutex mutex_;
    redisContext* context_ = nullptr;
    std::string last_error_;
    std::chrono::steady_clock::time_point next_retry_at_;
};

std::string serializeObjectInfo(const ObjectInfo& info);
std::optional<ObjectInfo> deserializeObjectInfo(const std::string& value);

} // namespace mini_oss
