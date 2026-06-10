#include "mini_oss/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace mini_oss {
namespace {

std::string trim(const std::string& value)
{
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        ++begin;
    }

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
        --end;
    }
    return std::string(begin, end);
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parseUnsigned(const std::string& value, std::uint64_t& parsed, std::string& error)
{
    try {
        std::size_t consumed = 0;
        parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            error = "invalid unsigned integer: " + value;
            return false;
        }
        return true;
    } catch (const std::exception&) {
        error = "invalid unsigned integer: " + value;
        return false;
    }
}

bool setConfigValue(AppConfig& config, const std::string& key, const std::string& value,
                    std::string& error)
{
    std::uint64_t parsed = 0;
    if (key == "port" || key == "server.port") {
        if (!parseUnsigned(value, parsed, error) || parsed == 0 || parsed > 65535) {
            error = "invalid port: " + value;
            return false;
        }
        config.port = static_cast<std::uint16_t>(parsed);
        return true;
    }

    if (key == "threads" || key == "worker_threads" || key == "server.threads"
        || key == "server.worker_threads") {
        if (!parseUnsigned(value, parsed, error) || parsed == 0) {
            error = "invalid thread count: " + value;
            return false;
        }
        config.worker_threads = static_cast<std::size_t>(parsed);
        return true;
    }

    if (key == "storage_dir" || key == "storage.dir" || key == "storage.root_dir"
        || key == "server.storage_dir") {
        if (value.empty()) {
            error = "storage_dir cannot be empty";
            return false;
        }
        config.storage_dir = value;
        return true;
    }

    if (key == "log_dir" || key == "logging.dir" || key == "logging.log_dir"
        || key == "server.log_dir") {
        if (value.empty()) {
            error = "log_dir cannot be empty";
            return false;
        }
        config.log_dir = value;
        return true;
    }

    if (key == "slow_request_ms" || key == "logging.slow_request_ms"
        || key == "server.slow_request_ms") {
        if (!parseUnsigned(value, parsed, error)) {
            return false;
        }
        config.slow_request_ms = parsed;
        return true;
    }

    if (key == "max_request_bytes" || key == "server.max_request_bytes"
        || key == "upload.max_request_bytes") {
        if (!parseUnsigned(value, parsed, error) || parsed == 0) {
            error = "invalid max_request_bytes: " + value;
            return false;
        }
        config.max_request_bytes = static_cast<std::size_t>(parsed);
        return true;
    }

    if (key == "max_upload_bytes" || key == "server.max_upload_bytes"
        || key == "upload.max_upload_bytes") {
        if (!parseUnsigned(value, parsed, error) || parsed == 0) {
            error = "invalid max_upload_bytes: " + value;
            return false;
        }
        config.max_upload_bytes = static_cast<std::size_t>(parsed);
        return true;
    }

    if (key == "stream_upload_threshold_bytes" || key == "server.stream_upload_threshold_bytes"
        || key == "upload.stream_upload_threshold_bytes") {
        if (!parseUnsigned(value, parsed, error)) {
            error = "invalid stream_upload_threshold_bytes: " + value;
            return false;
        }
        config.stream_upload_threshold_bytes = static_cast<std::size_t>(parsed);
        return true;
    }

    if (key == "auth_token" || key == "auth.token" || key == "server.auth_token") {
        config.auth_token = value;
        return true;
    }

    error = "unknown config key: " + key;
    return false;
}

} // namespace

bool loadConfigFile(const std::filesystem::path& path, AppConfig& config, std::string& error)
{
    std::ifstream in(path);
    if (!in) {
        error = "cannot open config file: " + path.string();
        return false;
    }

    std::string section;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = lowerCopy(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            error = "invalid config line " + std::to_string(line_no);
            return false;
        }

        std::string key = lowerCopy(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty()) {
            error = "empty config key at line " + std::to_string(line_no);
            return false;
        }
        if (!section.empty() && key.find('.') == std::string::npos) {
            key = section + "." + key;
        }

        if (!setConfigValue(config, key, value, error)) {
            error += " at line " + std::to_string(line_no);
            return false;
        }
    }

    return true;
}

} // namespace mini_oss
