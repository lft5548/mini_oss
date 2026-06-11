#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace mini_oss {

class Logger {
public:
    explicit Logger(std::filesystem::path log_dir, std::size_t queue_limit = 8192);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool ready() const;
    std::string lastError() const;
    std::uint64_t droppedCount() const;

    void info(const std::string& message);
    void error(const std::string& message);
    void access(const std::string& remote, const std::string& method, const std::string& path,
                int status_code, std::size_t response_bytes, std::uint64_t duration_ms);
    void slow(const std::string& remote, const std::string& method, const std::string& path,
              int status_code, std::uint64_t duration_ms);

private:
    enum class Target {
        Access,
        Error,
        Slow,
    };

    struct Entry {
        Target target;
        std::string line;
    };

    static std::string now();
    void enqueue(Target target, std::string line);
    void workerLoop();
    void writeLine(Target target, const std::string& line);
    void flushAll();
    void shutdown();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::filesystem::path log_dir_;
    std::ofstream access_log_;
    std::ofstream error_log_;
    std::ofstream slow_log_;
    std::deque<Entry> queue_;
    std::thread worker_;
    std::size_t queue_limit_ = 8192;
    bool stopping_ = false;
    std::atomic<std::uint64_t> dropped_count_ {0};
    std::string last_error_;
};

} // namespace mini_oss
