#include "mini_oss/logger.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace mini_oss {

Logger::Logger(std::filesystem::path log_dir, std::size_t queue_limit)
    : log_dir_(std::move(log_dir))
    , queue_limit_(queue_limit)
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
        return;
    }

    worker_ = std::thread([this] {
        workerLoop();
    });
}

Logger::~Logger()
{
    shutdown();
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

std::uint64_t Logger::droppedCount() const
{
    return dropped_count_.load(std::memory_order_relaxed);
}

void Logger::info(const std::string& message)
{
    enqueue(Target::Error, now() + " level=INFO msg=\"" + message + "\"");
}

void Logger::error(const std::string& message)
{
    enqueue(Target::Error, now() + " level=ERROR msg=\"" + message + "\"");
}

void Logger::access(const std::string& remote, const std::string& method, const std::string& path,
                    int status_code, std::size_t response_bytes, std::uint64_t duration_ms)
{
    std::ostringstream oss;
    oss << now() << " remote=" << remote << " method=" << method << " path=" << path
        << " status=" << status_code << " bytes=" << response_bytes
        << " duration_ms=" << duration_ms;
    enqueue(Target::Access, oss.str());
}

void Logger::slow(const std::string& remote, const std::string& method, const std::string& path,
                  int status_code, std::uint64_t duration_ms)
{
    std::ostringstream oss;
    oss << now() << " remote=" << remote << " method=" << method << " path=" << path
        << " status=" << status_code << " duration_ms=" << duration_ms;
    enqueue(Target::Slow, oss.str());
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

void Logger::enqueue(Target target, std::string line)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_error_.empty() || stopping_) {
            return;
        }
        if (queue_limit_ > 0 && queue_.size() >= queue_limit_) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(Entry {target, std::move(line)});
    }
    cv_.notify_one();
}

void Logger::workerLoop()
{
    std::deque<Entry> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });

            if (queue_.empty() && stopping_) {
                break;
            }
            batch.swap(queue_);
        }

        for (const auto& entry : batch) {
            writeLine(entry.target, entry.line);
        }
        batch.clear();
        flushAll();
    }

    const auto dropped = dropped_count_.load(std::memory_order_relaxed);
    if (dropped > 0) {
        writeLine(Target::Error, now() + " level=WARN msg=\"async logger dropped "
                  + std::to_string(dropped) + " log entries\"");
    }
    flushAll();
}

void Logger::writeLine(Target target, const std::string& line)
{
    std::ofstream* out = nullptr;
    if (target == Target::Access) {
        out = &access_log_;
    } else if (target == Target::Error) {
        out = &error_log_;
    } else {
        out = &slow_log_;
    }

    if (out != nullptr && *out) {
        *out << line << '\n';
    }
}

void Logger::flushAll()
{
    access_log_.flush();
    error_log_.flush();
    slow_log_.flush();
}

void Logger::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    } else {
        flushAll();
    }
}

} // namespace mini_oss
