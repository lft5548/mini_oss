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
    std::uint64_t slow_request_ms = 200;
    std::size_t max_request_bytes = 10 * 1024 * 1024;
    std::size_t max_upload_bytes = 128 * 1024 * 1024;
    std::size_t stream_upload_threshold_bytes = 1024 * 1024;
    std::string auth_token;
};

bool loadConfigFile(const std::filesystem::path& path, AppConfig& config, std::string& error);

} // namespace mini_oss
