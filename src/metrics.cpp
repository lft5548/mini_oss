#include "mini_oss/metrics.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace mini_oss {

void Metrics::connectionOpened()
{
    total_connections_.fetch_add(1, std::memory_order_relaxed);
    const auto active = active_connections_.fetch_add(1, std::memory_order_relaxed) + 1;
    updatePeakConnections(active);
}

void Metrics::connectionClosed()
{
    std::uint64_t current = active_connections_.load(std::memory_order_relaxed);
    while (current > 0
           && !active_connections_.compare_exchange_weak(current, current - 1,
                                                         std::memory_order_relaxed)) {
    }
}

void Metrics::connectionRejected()
{
    rejected_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::queueRejected()
{
    queue_rejections_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::requestTimedOut()
{
    request_timeouts_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::uploadTimedOut()
{
    upload_timeouts_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::recordObjectUpload(std::size_t body_bytes, bool streamed)
{
    object_upload_requests_.fetch_add(1, std::memory_order_relaxed);
    uploaded_bytes_.fetch_add(static_cast<std::uint64_t>(body_bytes), std::memory_order_relaxed);
    if (streamed) {
        streamed_upload_requests_.fetch_add(1, std::memory_order_relaxed);
        streamed_uploaded_bytes_.fetch_add(static_cast<std::uint64_t>(body_bytes),
                                           std::memory_order_relaxed);
    }
}

void Metrics::recordFileDownload(std::size_t body_bytes)
{
    file_download_requests_.fetch_add(1, std::memory_order_relaxed);
    streamed_downloaded_bytes_.fetch_add(static_cast<std::uint64_t>(body_bytes),
                                         std::memory_order_relaxed);
}

void Metrics::metadataCacheHit()
{
    metadata_cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::metadataCacheMiss()
{
    metadata_cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::metadataCacheError()
{
    metadata_cache_errors_.fetch_add(1, std::memory_order_relaxed);
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

    if (status_code >= 100 && status_code < 200) {
        status_1xx_.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 200 && status_code < 300) {
        status_2xx_.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 300 && status_code < 400) {
        status_3xx_.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 400 && status_code < 500) {
        status_4xx_.fetch_add(1, std::memory_order_relaxed);
    } else if (status_code >= 500 && status_code < 600) {
        status_5xx_.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(status_codes_mutex_);
        ++status_codes_[status_code];
    }

    request_bytes_.fetch_add(static_cast<std::uint64_t>(request_bytes), std::memory_order_relaxed);
    response_bytes_.fetch_add(static_cast<std::uint64_t>(response_bytes), std::memory_order_relaxed);
    total_latency_ms_.fetch_add(duration_ms, std::memory_order_relaxed);
}

MetricsSnapshot Metrics::snapshot() const
{
    MetricsSnapshot snapshot;
    snapshot.active_connections = active_connections_.load(std::memory_order_relaxed);
    snapshot.peak_active_connections = peak_active_connections_.load(std::memory_order_relaxed);
    snapshot.total_connections = total_connections_.load(std::memory_order_relaxed);
    snapshot.rejected_connections = rejected_connections_.load(std::memory_order_relaxed);
    snapshot.total_requests = total_requests_.load(std::memory_order_relaxed);
    snapshot.success_requests = success_requests_.load(std::memory_order_relaxed);
    snapshot.failed_requests = failed_requests_.load(std::memory_order_relaxed);
    snapshot.status_1xx = status_1xx_.load(std::memory_order_relaxed);
    snapshot.status_2xx = status_2xx_.load(std::memory_order_relaxed);
    snapshot.status_3xx = status_3xx_.load(std::memory_order_relaxed);
    snapshot.status_4xx = status_4xx_.load(std::memory_order_relaxed);
    snapshot.status_5xx = status_5xx_.load(std::memory_order_relaxed);
    snapshot.queue_rejections = queue_rejections_.load(std::memory_order_relaxed);
    snapshot.request_timeouts = request_timeouts_.load(std::memory_order_relaxed);
    snapshot.upload_timeouts = upload_timeouts_.load(std::memory_order_relaxed);
    snapshot.object_upload_requests = object_upload_requests_.load(std::memory_order_relaxed);
    snapshot.streamed_upload_requests = streamed_upload_requests_.load(std::memory_order_relaxed);
    snapshot.file_download_requests = file_download_requests_.load(std::memory_order_relaxed);
    snapshot.uploaded_bytes = uploaded_bytes_.load(std::memory_order_relaxed);
    snapshot.streamed_uploaded_bytes = streamed_uploaded_bytes_.load(std::memory_order_relaxed);
    snapshot.streamed_downloaded_bytes = streamed_downloaded_bytes_.load(std::memory_order_relaxed);
    snapshot.request_bytes = request_bytes_.load(std::memory_order_relaxed);
    snapshot.response_bytes = response_bytes_.load(std::memory_order_relaxed);
    snapshot.total_latency_ms = total_latency_ms_.load(std::memory_order_relaxed);
    snapshot.metadata_cache_hits = metadata_cache_hits_.load(std::memory_order_relaxed);
    snapshot.metadata_cache_misses = metadata_cache_misses_.load(std::memory_order_relaxed);
    snapshot.metadata_cache_errors = metadata_cache_errors_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(status_codes_mutex_);
        snapshot.status_codes = status_codes_;
    }
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
        << "\"peak_active_connections\":" << data.peak_active_connections << ","
        << "\"total_connections\":" << data.total_connections << ","
        << "\"rejected_connections\":" << data.rejected_connections << ","
        << "\"total_requests\":" << data.total_requests << ","
        << "\"success_requests\":" << data.success_requests << ","
        << "\"failed_requests\":" << data.failed_requests << ","
        << "\"status_1xx\":" << data.status_1xx << ","
        << "\"status_2xx\":" << data.status_2xx << ","
        << "\"status_3xx\":" << data.status_3xx << ","
        << "\"status_4xx\":" << data.status_4xx << ","
        << "\"status_5xx\":" << data.status_5xx << ","
        << "\"queue_rejections\":" << data.queue_rejections << ","
        << "\"request_timeouts\":" << data.request_timeouts << ","
        << "\"upload_timeouts\":" << data.upload_timeouts << ","
        << "\"object_upload_requests\":" << data.object_upload_requests << ","
        << "\"streamed_upload_requests\":" << data.streamed_upload_requests << ","
        << "\"file_download_requests\":" << data.file_download_requests << ","
        << "\"uploaded_bytes\":" << data.uploaded_bytes << ","
        << "\"streamed_uploaded_bytes\":" << data.streamed_uploaded_bytes << ","
        << "\"streamed_downloaded_bytes\":" << data.streamed_downloaded_bytes << ","
        << "\"request_bytes\":" << data.request_bytes << ","
        << "\"response_bytes\":" << data.response_bytes << ","
        << "\"total_latency_ms\":" << data.total_latency_ms << ","
        << "\"average_latency_ms\":" << data.average_latency_ms << ","
        << "\"metadata_cache_hits\":" << data.metadata_cache_hits << ","
        << "\"metadata_cache_misses\":" << data.metadata_cache_misses << ","
        << "\"metadata_cache_errors\":" << data.metadata_cache_errors << ","
        << "\"status_codes\":{";
    bool first_status = true;
    for (const auto& item : data.status_codes) {
        if (!first_status) {
            oss << ',';
        }
        first_status = false;
        oss << "\"" << item.first << "\":" << item.second;
    }
    oss << "}"
        << "}";
    return oss.str();
}

void Metrics::updatePeakConnections(std::uint64_t active_connections)
{
    auto current_peak = peak_active_connections_.load(std::memory_order_relaxed);
    while (active_connections > current_peak
           && !peak_active_connections_.compare_exchange_weak(
               current_peak, active_connections, std::memory_order_relaxed)) {
    }
}

} // namespace mini_oss
