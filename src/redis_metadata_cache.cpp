#include "mini_oss/redis_metadata_cache.h"

#include <hiredis/hiredis.h>

#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

namespace mini_oss {
namespace {

constexpr auto kReconnectDelay = std::chrono::seconds(1);

struct ReplyGuard {
    explicit ReplyGuard(redisReply* value)
        : reply(value)
    {
    }

    ~ReplyGuard()
    {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }

    ReplyGuard(const ReplyGuard&) = delete;
    ReplyGuard& operator=(const ReplyGuard&) = delete;

    redisReply* reply = nullptr;
};

std::string fieldEncode(const std::string& value)
{
    return std::to_string(value.size()) + ":" + value;
}

bool readField(const std::string& value, std::size_t& pos, std::string& field)
{
    const auto colon = value.find(':', pos);
    if (colon == std::string::npos || colon == pos) {
        return false;
    }

    const std::string length_text = value.substr(pos, colon - pos);
    char* end = nullptr;
    const auto length = std::strtoull(length_text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }

    const auto begin = colon + 1;
    if (begin + length > value.size()) {
        return false;
    }
    field = value.substr(begin, static_cast<std::size_t>(length));
    pos = begin + static_cast<std::size_t>(length);
    return true;
}

timeval timeoutFromMs(std::uint64_t timeout_ms)
{
    timeval timeout {};
    timeout.tv_sec = static_cast<time_t>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
    return timeout;
}

} // namespace

std::string serializeObjectInfo(const ObjectInfo& info)
{
    return fieldEncode(info.id)
        + fieldEncode(info.filename)
        + fieldEncode(info.path.string())
        + fieldEncode(std::to_string(info.size))
        + fieldEncode(info.sha256)
        + fieldEncode(info.created_at)
        + fieldEncode(std::to_string(info.owner_user_id))
        + fieldEncode(std::to_string(info.upload_count))
        + fieldEncode(std::to_string(info.download_count));
}

std::optional<ObjectInfo> deserializeObjectInfo(const std::string& value)
{
    std::vector<std::string> fields;
    fields.reserve(6);
    std::size_t pos = 0;
    while (pos < value.size()) {
        std::string field;
        if (!readField(value, pos, field)) {
            return std::nullopt;
        }
        fields.push_back(std::move(field));
    }
    if (fields.size() != 6 && fields.size() != 9) {
        return std::nullopt;
    }

    char* end = nullptr;
    const auto size = std::strtoull(fields[3].c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }

    std::uint64_t upload_count = 1;
    std::uint64_t download_count = 0;
    int owner_user_id = 0;
    if (fields.size() == 9) {
        char* owner_end = nullptr;
        const auto owner = std::strtol(fields[6].c_str(), &owner_end, 10);
        char* upload_end = nullptr;
        upload_count = std::strtoull(fields[7].c_str(), &upload_end, 10);
        char* download_end = nullptr;
        download_count = std::strtoull(fields[8].c_str(), &download_end, 10);
        if (owner_end == nullptr || *owner_end != '\0' || upload_end == nullptr
            || *upload_end != '\0' || download_end == nullptr || *download_end != '\0') {
            return std::nullopt;
        }
        owner_user_id = static_cast<int>(owner);
    }

    ObjectInfo info;
    info.id = fields[0];
    info.filename = fields[1];
    info.path = fields[2];
    info.size = static_cast<std::uint64_t>(size);
    info.sha256 = fields[4];
    info.created_at = fields[5];
    info.owner_user_id = owner_user_id;
    info.upload_count = upload_count;
    info.download_count = download_count;
    return info;
}

RedisMetadataCache::RedisMetadataCache(RedisConfig config, Metrics* metrics)
    : config_(std::move(config))
    , metrics_(metrics)
{
}

RedisMetadataCache::~RedisMetadataCache()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

bool RedisMetadataCache::enabled() const
{
    return config_.enabled;
}

std::string RedisMetadataCache::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::optional<ObjectInfo> RedisMetadataCache::getObject(const std::string& id)
{
    if (!enabled()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto cached = getStringLocked(objectKey(id));
    if (!cached.has_value()) {
        return std::nullopt;
    }

    auto info = deserializeObjectInfo(cached.value());
    if (!info.has_value()) {
        commandErrorLocked("invalid cached object metadata");
        return std::nullopt;
    }
    return info;
}

std::optional<std::string> RedisMetadataCache::getShaIndex(const std::string& sha256,
                                                           std::uint64_t size)
{
    if (!enabled()) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return getStringLocked(shaKey(sha256, size));
}

void RedisMetadataCache::putObject(const ObjectInfo& info)
{
    if (!enabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    setStringLocked(objectKey(info.id), serializeObjectInfo(info));
}

void RedisMetadataCache::putShaIndex(const std::string& sha256, std::uint64_t size,
                                     const std::string& id)
{
    if (!enabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    setStringLocked(shaKey(sha256, size), id);
}

void RedisMetadataCache::deleteObject(const ObjectInfo& info)
{
    if (!enabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    delKeyLocked(objectKey(info.id));
    delKeyLocked(shaKey(info.sha256, info.size));
}

void RedisMetadataCache::deleteShaIndex(const std::string& sha256, std::uint64_t size)
{
    if (!enabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    delKeyLocked(shaKey(sha256, size));
}

bool RedisMetadataCache::ensureConnectedLocked()
{
    if (!enabled()) {
        return false;
    }
    if (context_ != nullptr && context_->err == 0) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (next_retry_at_ > now) {
        return false;
    }

    closeLocked();
    auto connect_timeout = timeoutFromMs(config_.connect_timeout_ms);
    context_ = redisConnectWithTimeout(config_.host.c_str(), config_.port, connect_timeout);
    if (context_ == nullptr) {
        commandErrorLocked("redis context allocation failed");
        return false;
    }
    if (context_->err != 0) {
        commandErrorLocked(context_->errstr[0] == '\0' ? "redis connect failed" : context_->errstr);
        return false;
    }

    auto io_timeout = timeoutFromMs(config_.io_timeout_ms);
    if (redisSetTimeout(context_, io_timeout) != REDIS_OK) {
        commandErrorLocked(context_->errstr[0] == '\0' ? "redis set timeout failed" : context_->errstr);
        return false;
    }

    if (!authenticateLocked()) {
        return false;
    }
    if (!selectDbLocked()) {
        return false;
    }
    return true;
}

bool RedisMetadataCache::authenticateLocked()
{
    if (config_.password.empty()) {
        return true;
    }

    ReplyGuard guard(static_cast<redisReply*>(redisCommand(context_, "AUTH %s", config_.password.c_str())));
    if (guard.reply == nullptr || guard.reply->type == REDIS_REPLY_ERROR) {
        commandErrorLocked(guard.reply == nullptr ? "redis AUTH failed" : guard.reply->str);
        return false;
    }
    return true;
}

bool RedisMetadataCache::selectDbLocked()
{
    if (config_.db == 0) {
        return true;
    }

    ReplyGuard guard(static_cast<redisReply*>(redisCommand(context_, "SELECT %u", config_.db)));
    if (guard.reply == nullptr || guard.reply->type == REDIS_REPLY_ERROR) {
        commandErrorLocked(guard.reply == nullptr ? "redis SELECT failed" : guard.reply->str);
        return false;
    }
    return true;
}

void RedisMetadataCache::closeLocked()
{
    if (context_ != nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }
}

void RedisMetadataCache::commandErrorLocked(const std::string& message)
{
    last_error_ = message;
    next_retry_at_ = std::chrono::steady_clock::now() + kReconnectDelay;
    if (metrics_ != nullptr) {
        metrics_->metadataCacheError();
    }
    closeLocked();
}

void RedisMetadataCache::postponeAfterInvalidationFailureLocked()
{
    if (config_.ttl_seconds == 0) {
        next_retry_at_ = std::chrono::steady_clock::time_point::max();
        return;
    }
    next_retry_at_ = std::chrono::steady_clock::now()
        + std::chrono::seconds(config_.ttl_seconds);
}

std::optional<std::string> RedisMetadataCache::getStringLocked(const std::string& key)
{
    if (!ensureConnectedLocked()) {
        return std::nullopt;
    }

    ReplyGuard guard(static_cast<redisReply*>(redisCommand(context_, "GET %s", key.c_str())));
    if (guard.reply == nullptr) {
        commandErrorLocked(context_ == nullptr || context_->errstr[0] == '\0'
                               ? "redis GET failed"
                               : context_->errstr);
        return std::nullopt;
    }
    if (guard.reply->type == REDIS_REPLY_NIL) {
        if (metrics_ != nullptr) {
            metrics_->metadataCacheMiss();
        }
        return std::nullopt;
    }
    if (guard.reply->type != REDIS_REPLY_STRING) {
        commandErrorLocked("redis GET returned unexpected reply type");
        return std::nullopt;
    }

    if (metrics_ != nullptr) {
        metrics_->metadataCacheHit();
    }
    return std::string(guard.reply->str, static_cast<std::size_t>(guard.reply->len));
}

bool RedisMetadataCache::setStringLocked(const std::string& key, const std::string& value)
{
    if (!ensureConnectedLocked()) {
        return false;
    }

    redisReply* raw_reply = nullptr;
    if (config_.ttl_seconds == 0) {
        raw_reply = static_cast<redisReply*>(redisCommand(context_, "SET %s %b", key.c_str(),
                                                          value.data(), value.size()));
    } else {
        raw_reply = static_cast<redisReply*>(redisCommand(context_, "SETEX %s %llu %b", key.c_str(),
                                                          static_cast<unsigned long long>(config_.ttl_seconds),
                                                          value.data(), value.size()));
    }
    ReplyGuard guard(raw_reply);
    if (guard.reply == nullptr || guard.reply->type == REDIS_REPLY_ERROR) {
        commandErrorLocked(guard.reply == nullptr ? "redis SET failed" : guard.reply->str);
        return false;
    }
    return true;
}

void RedisMetadataCache::delKeyLocked(const std::string& key)
{
    if (!ensureConnectedLocked()) {
        return;
    }

    ReplyGuard guard(static_cast<redisReply*>(redisCommand(context_, "DEL %s", key.c_str())));
    if (guard.reply == nullptr || guard.reply->type == REDIS_REPLY_ERROR) {
        commandErrorLocked(guard.reply == nullptr ? "redis DEL failed" : guard.reply->str);
        postponeAfterInvalidationFailureLocked();
    }
}

std::string RedisMetadataCache::objectKey(const std::string& id) const
{
    return config_.key_prefix + ":object:" + id;
}

std::string RedisMetadataCache::shaKey(const std::string& sha256, std::uint64_t size) const
{
    return config_.key_prefix + ":sha:" + sha256 + ":" + std::to_string(size);
}

} // namespace mini_oss
