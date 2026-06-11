#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mini_oss {

struct RedisConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string password;
    std::uint32_t db = 0;
    std::string key_prefix = "mini_oss";
    std::uint64_t ttl_seconds = 300;
    std::uint64_t connect_timeout_ms = 100;
    std::uint64_t io_timeout_ms = 100;
};

struct AppConfig {
    std::uint16_t port = 8080;
    std::size_t worker_threads = 0;
    std::filesystem::path storage_dir = "storage";
    std::filesystem::path log_dir = "logs";
    std::size_t log_queue_limit = 8192;
    std::uint64_t slow_request_ms = 200;
    std::size_t max_connections = 1024;
    std::size_t thread_queue_limit = 1024;
    std::size_t max_request_bytes = 10 * 1024 * 1024;
    std::size_t max_upload_bytes = 128 * 1024 * 1024;
    std::size_t stream_upload_threshold_bytes = 1024 * 1024;
    std::uint64_t request_timeout_ms = 5000;
    std::uint64_t upload_timeout_ms = 30000;
    std::string auth_token;
    RedisConfig redis;
};

bool loadConfigFile(const std::filesystem::path& path, AppConfig& config, std::string& error);

} // namespace mini_oss
