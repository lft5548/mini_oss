#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mini_oss {

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
};

bool loadConfigFile(const std::filesystem::path& path, AppConfig& config, std::string& error);

} // namespace mini_oss
