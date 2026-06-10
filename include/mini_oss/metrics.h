#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mini_oss {

struct MetricsSnapshot {
    std::uint64_t active_connections = 0;
    std::uint64_t total_requests = 0;
    std::uint64_t success_requests = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t request_bytes = 0;
    std::uint64_t response_bytes = 0;
    std::uint64_t total_latency_ms = 0;
    double average_latency_ms = 0.0;
};

class Metrics {
public:
    void connectionOpened();
    void connectionClosed();
    void recordRequest(int status_code, std::size_t request_bytes, std::size_t response_bytes,
                       std::uint64_t duration_ms);

    MetricsSnapshot snapshot() const;
    std::string toJson() const;

private:
    std::atomic<std::uint64_t> active_connections_ {0};
    std::atomic<std::uint64_t> total_requests_ {0};
    std::atomic<std::uint64_t> success_requests_ {0};
    std::atomic<std::uint64_t> failed_requests_ {0};
    std::atomic<std::uint64_t> request_bytes_ {0};
    std::atomic<std::uint64_t> response_bytes_ {0};
    std::atomic<std::uint64_t> total_latency_ms_ {0};
};

} // namespace mini_oss
