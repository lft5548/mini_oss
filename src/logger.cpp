#include "mini_oss/logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace mini_oss {

Logger::Logger(std::filesystem::path log_dir)
    : log_dir_(std::move(log_dir))
{
    std::error_code ec;
    std::filesystem::create_directories(log_dir_, ec);
    if (ec) {
        last_error_ = "cannot create log directory: " + ec.message();
        return;
    }

    access_log_.open(log_dir_ / "access.log", std::ios::app);
    error_log_.open(log_dir_ / "error.log", std::ios::app);
    slow_log_.open(log_dir_ / "slow.log", std::ios::app);
    if (!access_log_ || !error_log_ || !slow_log_) {
        last_error_ = "cannot open log files under: " + log_dir_.string();
    }
}

Logger::~Logger()
{
    std::lock_guard<std::mutex> lock(mutex_);
    access_log_.flush();
    error_log_.flush();
    slow_log_.flush();
}

bool Logger::ready() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_.empty();
}

std::string Logger::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void Logger::info(const std::string& message)
{
    writeLine(error_log_, now() + " level=INFO msg=\"" + message + "\"");
}

void Logger::error(const std::string& message)
{
    writeLine(error_log_, now() + " level=ERROR msg=\"" + message + "\"");
}

void Logger::access(const std::string& remote, const std::string& method, const std::string& path,
                    int status_code, std::size_t response_bytes, std::uint64_t duration_ms)
{
    std::ostringstream oss;
    oss << now() << " remote=" << remote << " method=" << method << " path=" << path
        << " status=" << status_code << " bytes=" << response_bytes
        << " duration_ms=" << duration_ms;
    writeLine(access_log_, oss.str());
}

void Logger::slow(const std::string& remote, const std::string& method, const std::string& path,
                  int status_code, std::uint64_t duration_ms)
{
    std::ostringstream oss;
    oss << now() << " remote=" << remote << " method=" << method << " path=" << path
        << " status=" << status_code << " duration_ms=" << duration_ms;
    writeLine(slow_log_, oss.str());
}

std::string Logger::now()
{
    const auto current = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(current);
    std::tm tm {};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void Logger::writeLine(std::ofstream& out, const std::string& line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out) {
        return;
    }
    out << line << '\n';
    out.flush();
}

} // namespace mini_oss
