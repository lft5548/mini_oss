#include "mini_oss/metrics.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace mini_oss {

void Metrics::connectionOpened()
{
    active_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::connectionClosed()
{
    std::uint64_t current = active_connections_.load(std::memory_order_relaxed);
    while (current > 0
           && !active_connections_.compare_exchange_weak(current, current - 1,
                                                         std::memory_order_relaxed)) {
    }
}

void Metrics::recordRequest(int status_code, std::size_t request_bytes, std::size_t response_bytes,
                            std::uint64_t duration_ms)
{
    total_requests_.fetch_add(1, std::memory_order_relaxed);
    if (status_code >= 200 && status_code < 400) {
        success_requests_.fetch_add(1, std::memory_order_relaxed);
    } else {
        failed_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    request_bytes_.fetch_add(static_cast<std::uint64_t>(request_bytes), std::memory_order_relaxed);
    response_bytes_.fetch_add(static_cast<std::uint64_t>(response_bytes), std::memory_order_relaxed);
    total_latency_ms_.fetch_add(duration_ms, std::memory_order_relaxed);
}

MetricsSnapshot Metrics::snapshot() const
{
    MetricsSnapshot snapshot;
    snapshot.active_connections = active_connections_.load(std::memory_order_relaxed);
    snapshot.total_requests = total_requests_.load(std::memory_order_relaxed);
    snapshot.success_requests = success_requests_.load(std::memory_order_relaxed);
    snapshot.failed_requests = failed_requests_.load(std::memory_order_relaxed);
    snapshot.request_bytes = request_bytes_.load(std::memory_order_relaxed);
    snapshot.response_bytes = response_bytes_.load(std::memory_order_relaxed);
    snapshot.total_latency_ms = total_latency_ms_.load(std::memory_order_relaxed);
    if (snapshot.total_requests > 0) {
        snapshot.average_latency_ms =
            static_cast<double>(snapshot.total_latency_ms) / snapshot.total_requests;
    }
    return snapshot;
}

std::string Metrics::toJson() const
{
    const auto data = snapshot();
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2)
        << "{"
        << "\"active_connections\":" << data.active_connections << ","
        << "\"total_requests\":" << data.total_requests << ","
        << "\"success_requests\":" << data.success_requests << ","
        << "\"failed_requests\":" << data.failed_requests << ","
        << "\"request_bytes\":" << data.request_bytes << ","
        << "\"response_bytes\":" << data.response_bytes << ","
        << "\"total_latency_ms\":" << data.total_latency_ms << ","
        << "\"average_latency_ms\":" << data.average_latency_ms
        << "}";
    return oss.str();
}

} // namespace mini_oss
