#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace mini_oss {

class Logger {
public:
    explicit Logger(std::filesystem::path log_dir);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool ready() const;
    std::string lastError() const;

    void info(const std::string& message);
    void error(const std::string& message);
    void access(const std::string& remote, const std::string& method, const std::string& path,
                int status_code, std::size_t response_bytes, std::uint64_t duration_ms);
    void slow(const std::string& remote, const std::string& method, const std::string& path,
              int status_code, std::uint64_t duration_ms);

private:
    static std::string now();
    void writeLine(std::ofstream& out, const std::string& line);

    mutable std::mutex mutex_;
    std::filesystem::path log_dir_;
    std::ofstream access_log_;
    std::ofstream error_log_;
    std::ofstream slow_log_;
    std::string last_error_;
};

} // namespace mini_oss
