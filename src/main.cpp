#include "mini_oss/config.h"
#include "mini_oss/http_server.h"
#include "mini_oss/logger.h"
#include "mini_oss/version.h"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {
std::atomic_bool g_running {true};

void handleSignal(int)
{
    g_running = false;
}

bool parseUnsigned(const std::string& value, std::uint64_t& parsed)
{
    try {
        std::size_t consumed = 0;
        parsed = std::stoull(value, &consumed);
        return consumed == value.size();
    } catch (const std::exception&) {
        return false;
    }
}

void printUsage()
{
    std::cout << "Usage: mini_oss [--config config.ini] [--port 8080] [--threads 4] "
                 "[--storage-dir storage] [--log-dir logs] [--log-queue-limit 8192] "
                 "[--slow-request-ms 200] [--max-connections 1024] [--thread-queue-limit 1024] "
                 "[--max-request-bytes bytes] [--max-upload-bytes bytes] "
                 "[--stream-upload-threshold-bytes bytes] [--request-timeout-ms 5000] "
                 "[--upload-timeout-ms 30000] [--auth-token token] "
                 "[--redis-enabled true|false] [--redis-host 127.0.0.1] "
                 "[--redis-port 6379] [--redis-db 0] [--redis-key-prefix mini_oss] "
                 "[--redis-ttl-seconds 300] [--version]\n";
}
}

int main(int argc, char* argv[])
{
    std::string config_path = "config.ini";
    bool config_path_explicit = false;
    std::optional<std::uint16_t> port_override;
    std::optional<std::size_t> worker_threads_override;
    std::optional<std::filesystem::path> storage_dir_override;
    std::optional<std::filesystem::path> log_dir_override;
    std::optional<std::size_t> log_queue_limit_override;
    std::optional<std::uint64_t> slow_request_ms_override;
    std::optional<std::size_t> max_connections_override;
    std::optional<std::size_t> thread_queue_limit_override;
    std::optional<std::size_t> max_request_bytes_override;
    std::optional<std::size_t> max_upload_bytes_override;
    std::optional<std::size_t> stream_upload_threshold_bytes_override;
    std::optional<std::uint64_t> request_timeout_ms_override;
    std::optional<std::uint64_t> upload_timeout_ms_override;
    std::optional<std::string> auth_token_override;
    std::optional<bool> redis_enabled_override;
    std::optional<std::string> redis_host_override;
    std::optional<std::uint16_t> redis_port_override;
    std::optional<std::string> redis_password_override;
    std::optional<std::uint32_t> redis_db_override;
    std::optional<std::string> redis_key_prefix_override;
    std::optional<std::uint64_t> redis_ttl_seconds_override;
    std::optional<std::uint64_t> redis_connect_timeout_ms_override;
    std::optional<std::uint64_t> redis_io_timeout_ms_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            config_path_explicit = true;
        } else if (arg == "--port" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value) || value == 0 || value > 65535) {
                std::cerr << "Invalid port\n";
                return 1;
            }
            port_override = static_cast<std::uint16_t>(value);
        } else if (arg == "--threads" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value) || value == 0) {
                std::cerr << "Invalid thread count\n";
                return 1;
            }
            worker_threads_override = static_cast<std::size_t>(value);
        } else if (arg == "--storage-dir" && i + 1 < argc) {
            storage_dir_override = argv[++i];
        } else if (arg == "--log-dir" && i + 1 < argc) {
            log_dir_override = argv[++i];
        } else if (arg == "--log-queue-limit" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid log queue limit\n";
                return 1;
            }
            log_queue_limit_override = static_cast<std::size_t>(value);
        } else if (arg == "--slow-request-ms" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid slow request threshold\n";
                return 1;
            }
            slow_request_ms_override = value;
        } else if (arg == "--max-connections" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value) || value == 0) {
                std::cerr << "Invalid max connections\n";
                return 1;
            }
            max_connections_override = static_cast<std::size_t>(value);
        } else if (arg == "--thread-queue-limit" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid thread queue limit\n";
                return 1;
            }
            thread_queue_limit_override = static_cast<std::size_t>(value);
        } else if (arg == "--max-request-bytes" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value) || value == 0) {
                std::cerr << "Invalid max request bytes\n";
                return 1;
            }
            max_request_bytes_override = static_cast<std::size_t>(value);
        } else if (arg == "--max-upload-bytes" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value) || value == 0) {
                std::cerr << "Invalid max upload bytes\n";
                return 1;
            }
            max_upload_bytes_override = static_cast<std::size_t>(value);
        } else if (arg == "--stream-upload-threshold-bytes" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid stream upload threshold bytes\n";
                return 1;
            }
            stream_upload_threshold_bytes_override = static_cast<std::size_t>(value);
        } else if (arg == "--request-timeout-ms" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid request timeout\n";
                return 1;
            }
            request_timeout_ms_override = value;
        } else if (arg == "--upload-timeout-ms" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid upload timeout\n";
                return 1;
            }
            upload_timeout_ms_override = value;
        } else if (arg == "--auth-token" && i + 1 < argc) {
            auth_token_override = argv[++i];
        } else if (arg == "--redis-enabled" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "1" || value == "true" || value == "yes" || value == "on") {
                redis_enabled_override = true;
            } else if (value == "0" || value == "false" || value == "no" || value == "off") {
                redis_enabled_override = false;
            } else {
                std::cerr << "Invalid redis enabled flag\n";
                return 1;
            }
        } else if (arg == "--redis-host" && i + 1 < argc) {
            redis_host_override = argv[++i];
        } else if (arg == "--redis-port" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value) || value == 0 || value > 65535) {
                std::cerr << "Invalid redis port\n";
                return 1;
            }
            redis_port_override = static_cast<std::uint16_t>(value);
        } else if (arg == "--redis-password" && i + 1 < argc) {
            redis_password_override = argv[++i];
        } else if (arg == "--redis-db" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid redis db\n";
                return 1;
            }
            redis_db_override = static_cast<std::uint32_t>(value);
        } else if (arg == "--redis-key-prefix" && i + 1 < argc) {
            redis_key_prefix_override = argv[++i];
        } else if (arg == "--redis-ttl-seconds" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid redis ttl seconds\n";
                return 1;
            }
            redis_ttl_seconds_override = value;
        } else if (arg == "--redis-connect-timeout-ms" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid redis connect timeout\n";
                return 1;
            }
            redis_connect_timeout_ms_override = value;
        } else if (arg == "--redis-io-timeout-ms" && i + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++i], value)) {
                std::cerr << "Invalid redis io timeout\n";
                return 1;
            }
            redis_io_timeout_ms_override = value;
        } else if (arg == "--version") {
            std::cout << "mini_oss " << MINI_OSS_VERSION << '\n';
            return 0;
        } else if (arg == "--help") {
            printUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            printUsage();
            return 1;
        }
    }

    mini_oss::AppConfig config;
    std::string config_error;
    if (std::filesystem::exists(config_path)) {
        if (!mini_oss::loadConfigFile(config_path, config, config_error)) {
            std::cerr << "Config error: " << config_error << '\n';
            return 1;
        }
    } else if (config_path_explicit) {
        std::cerr << "Config error: cannot open config file: " << config_path << '\n';
        return 1;
    }

    if (port_override.has_value()) {
        config.port = port_override.value();
    }
    if (worker_threads_override.has_value()) {
        config.worker_threads = worker_threads_override.value();
    }
    if (storage_dir_override.has_value()) {
        config.storage_dir = storage_dir_override.value();
    }
    if (log_dir_override.has_value()) {
        config.log_dir = log_dir_override.value();
    }
    if (log_queue_limit_override.has_value()) {
        config.log_queue_limit = log_queue_limit_override.value();
    }
    if (slow_request_ms_override.has_value()) {
        config.slow_request_ms = slow_request_ms_override.value();
    }
    if (max_connections_override.has_value()) {
        config.max_connections = max_connections_override.value();
    }
    if (thread_queue_limit_override.has_value()) {
        config.thread_queue_limit = thread_queue_limit_override.value();
    }
    if (max_request_bytes_override.has_value()) {
        config.max_request_bytes = max_request_bytes_override.value();
    }
    if (max_upload_bytes_override.has_value()) {
        config.max_upload_bytes = max_upload_bytes_override.value();
    }
    if (stream_upload_threshold_bytes_override.has_value()) {
        config.stream_upload_threshold_bytes = stream_upload_threshold_bytes_override.value();
    }
    if (request_timeout_ms_override.has_value()) {
        config.request_timeout_ms = request_timeout_ms_override.value();
    }
    if (upload_timeout_ms_override.has_value()) {
        config.upload_timeout_ms = upload_timeout_ms_override.value();
    }
    if (auth_token_override.has_value()) {
        config.auth_token = auth_token_override.value();
    }
    if (redis_enabled_override.has_value()) {
        config.redis.enabled = redis_enabled_override.value();
    }
    if (redis_host_override.has_value()) {
        config.redis.host = redis_host_override.value();
    }
    if (redis_port_override.has_value()) {
        config.redis.port = redis_port_override.value();
    }
    if (redis_password_override.has_value()) {
        config.redis.password = redis_password_override.value();
    }
    if (redis_db_override.has_value()) {
        config.redis.db = redis_db_override.value();
    }
    if (redis_key_prefix_override.has_value()) {
        config.redis.key_prefix = redis_key_prefix_override.value();
    }
    if (redis_ttl_seconds_override.has_value()) {
        config.redis.ttl_seconds = redis_ttl_seconds_override.value();
    }
    if (redis_connect_timeout_ms_override.has_value()) {
        config.redis.connect_timeout_ms = redis_connect_timeout_ms_override.value();
    }
    if (redis_io_timeout_ms_override.has_value()) {
        config.redis.io_timeout_ms = redis_io_timeout_ms_override.value();
    }

    mini_oss::Logger logger(config.log_dir, config.log_queue_limit);
    if (!logger.ready()) {
        std::cerr << "Logger error: " << logger.lastError() << '\n';
        return 1;
    }

    std::cout << "Mini-OSS starting...\n";
    std::cout << "Version: " << MINI_OSS_VERSION << '\n';
    std::cout << "Config: " << config_path << '\n';
    std::cout << "Storage: " << config.storage_dir << '\n';
    std::cout << "Logs: " << config.log_dir << '\n';
    std::cout << "Log queue limit: " << config.log_queue_limit << '\n';
    std::cout << "Redis cache: " << (config.redis.enabled ? "enabled" : "disabled") << '\n';
    logger.info("Mini-OSS starting");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    mini_oss::HttpServer server(config.port, config.worker_threads, config.storage_dir, logger,
                                config.slow_request_ms, config.auth_token,
                                config.max_request_bytes, config.max_upload_bytes,
                                config.stream_upload_threshold_bytes,
                                config.max_connections, config.thread_queue_limit,
                                config.request_timeout_ms, config.upload_timeout_ms,
                                config.redis);
    if (!server.start()) {
        return 1;
    }

    server.run([] {
        return g_running.load();
    });

    logger.info("Mini-OSS stopped");
    std::cout << "Mini-OSS stopped.\n";
    return 0;
}
