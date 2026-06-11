#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mini_oss {

struct MetricsSnapshot {
    std::uint64_t active_connections = 0;
    std::uint64_t peak_active_connections = 0;
    std::uint64_t total_connections = 0;
    std::uint64_t rejected_connections = 0;
    std::uint64_t total_requests = 0;
    std::uint64_t success_requests = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t status_1xx = 0;
    std::uint64_t status_2xx = 0;
    std::uint64_t status_3xx = 0;
    std::uint64_t status_4xx = 0;
    std::uint64_t status_5xx = 0;
    std::uint64_t queue_rejections = 0;
    std::uint64_t request_timeouts = 0;
    std::uint64_t upload_timeouts = 0;
    std::uint64_t object_upload_requests = 0;
    std::uint64_t streamed_upload_requests = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t streamed_uploaded_bytes = 0;
    std::uint64_t request_bytes = 0;
    std::uint64_t response_bytes = 0;
    std::uint64_t total_latency_ms = 0;
    double average_latency_ms = 0.0;
};

class Metrics {
public:
    void connectionOpened();
    void connectionClosed();
    void connectionRejected();
    void queueRejected();
    void requestTimedOut();
    void uploadTimedOut();
    void recordObjectUpload(std::size_t body_bytes, bool streamed);
    void recordRequest(int status_code, std::size_t request_bytes, std::size_t response_bytes,
                       std::uint64_t duration_ms);

    MetricsSnapshot snapshot() const;
    std::string toJson() const;

private:
    void updatePeakConnections(std::uint64_t active_connections);

    std::atomic<std::uint64_t> active_connections_ {0};
    std::atomic<std::uint64_t> peak_active_connections_ {0};
    std::atomic<std::uint64_t> total_connections_ {0};
    std::atomic<std::uint64_t> rejected_connections_ {0};
    std::atomic<std::uint64_t> total_requests_ {0};
    std::atomic<std::uint64_t> success_requests_ {0};
    std::atomic<std::uint64_t> failed_requests_ {0};
    std::atomic<std::uint64_t> status_1xx_ {0};
    std::atomic<std::uint64_t> status_2xx_ {0};
    std::atomic<std::uint64_t> status_3xx_ {0};
    std::atomic<std::uint64_t> status_4xx_ {0};
    std::atomic<std::uint64_t> status_5xx_ {0};
    std::atomic<std::uint64_t> queue_rejections_ {0};
    std::atomic<std::uint64_t> request_timeouts_ {0};
    std::atomic<std::uint64_t> upload_timeouts_ {0};
    std::atomic<std::uint64_t> object_upload_requests_ {0};
    std::atomic<std::uint64_t> streamed_upload_requests_ {0};
    std::atomic<std::uint64_t> uploaded_bytes_ {0};
    std::atomic<std::uint64_t> streamed_uploaded_bytes_ {0};
    std::atomic<std::uint64_t> request_bytes_ {0};
    std::atomic<std::uint64_t> response_bytes_ {0};
    std::atomic<std::uint64_t> total_latency_ms_ {0};
};

} // namespace mini_oss
