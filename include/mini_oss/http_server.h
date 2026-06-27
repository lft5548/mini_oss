#pragma once

#include "mini_oss/auth_service.h"
#include "mini_oss/config.h"
#include "mini_oss/logger.h"
#include "mini_oss/metrics.h"
#include "mini_oss/object_store.h"
#include "mini_oss/router.h"
#include "mini_oss/thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>

namespace mini_oss {

class HttpServer {
public:
    HttpServer(std::uint16_t port, std::size_t worker_threads, std::filesystem::path storage_dir,
               Logger& logger, std::uint64_t slow_request_ms = 200, std::string auth_token = {},
               std::size_t max_request_bytes = 10 * 1024 * 1024,
               std::size_t max_upload_bytes = 128 * 1024 * 1024,
               std::size_t stream_upload_threshold_bytes = 1024 * 1024,
               std::size_t max_connections = 1024, std::size_t thread_queue_limit = 1024,
               std::uint64_t request_timeout_ms = 5000,
               std::uint64_t upload_timeout_ms = 30000,
               RedisConfig redis_config = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void run(const std::function<bool()>& keep_running);
    void stop();

private:
    void acceptClients();
    void handleWakeup();
    void handleClientRead(int client_fd);
    struct ClientState {
        std::string buffer;
        std::string remote_addr;
        std::chrono::steady_clock::time_point started_at;
        std::chrono::steady_clock::time_point last_activity_at;
        std::uint64_t generation = 0;
        bool processing = false;
        bool header_parsed = false;
        bool streaming_upload = false;
        bool writing_response = false;
        std::uint64_t content_length = 0;
        std::uint64_t received_body_bytes = 0;
        std::size_t request_bytes = 0;
        HttpRequest header_request;
        std::filesystem::path temp_upload_path;
        int temp_upload_fd = -1;
        bool owns_temp_upload = false;
        std::string response_buffer;
        std::size_t response_buffer_sent = 0;
        std::shared_ptr<HttpFileBody> response_file;
        std::uint64_t response_file_offset = 0;
        std::uint64_t response_file_remaining = 0;
        std::size_t response_total_bytes = 0;
        int response_status_code = 0;
        std::string response_method;
        std::string response_path;
        std::string response_remote_addr;
        std::size_t response_request_bytes = 0;
        std::chrono::steady_clock::time_point response_started_at;
    };

    struct PendingResponse {
        int client_fd = -1;
        std::uint64_t generation = 0;
        HttpResponse response;
        std::string remote_addr;
        std::string method;
        std::string path;
        std::chrono::steady_clock::time_point started_at;
        std::size_t request_bytes = 0;
    };

    void submitRequest(int client_fd, std::uint64_t generation, std::string remote_addr,
                       std::chrono::steady_clock::time_point started_at, HttpRequest request,
                       std::size_t request_bytes);
    void enqueueResponse(int client_fd, std::uint64_t generation, HttpResponse response,
                         std::string remote_addr, std::string method, std::string path,
                         std::chrono::steady_clock::time_point started_at,
                         std::size_t request_bytes);
    void sendCompletedResponses();
    void beginResponse(PendingResponse response);
    void handleClientWrite(int client_fd);
    void finishResponse(int client_fd);
    void closeClient(int client_fd);
    void closeTimedOutClients();
    void logServerError(const std::string& message);
    void sendImmediateResponse(int client_fd, const HttpResponse& response,
                               const std::string& method, const std::string& path,
                               std::size_t request_bytes);
    bool shouldStreamUpload(const HttpRequest& request, std::uint64_t content_length) const;
    bool beginStreamingUpload(int client_fd, ClientState& state, HttpRequest request,
                              std::uint64_t content_length, std::size_t request_bytes);
    bool writeStreamingUpload(ClientState& state, const char* data, std::size_t size);
    void cleanupClientUpload(ClientState& state);
    void submitStreamingUpload(int client_fd, ClientState& state);
    bool requiresAuth(const HttpRequest& request) const;
    std::optional<HttpResponse> authorizeRequest(HttpRequest& request);
    bool isLegacyTokenAuthorized(const HttpRequest& request) const;
    HttpResponse handleAdminUsers(const HttpRequest& request);
    HttpResponse handleUpdateUserRoles(const HttpRequest& request);
    HttpResponse handleAuditLogs(const HttpRequest& request);
    HttpResponse handleAdminStatsOverview(const HttpRequest& request);
    HttpResponse handleAdminStatsStatusCodes(const HttpRequest& request);
    HttpResponse handleAdminStatsRedis(const HttpRequest& request);
    void recordAudit(const HttpRequest& request, const HttpResponse& response,
                     const std::string& remote_addr,
                     std::chrono::steady_clock::time_point started_at);
    void logAccess(const std::string& remote, const std::string& method,
                   const std::string& path, int status_code,
                   std::size_t request_bytes, std::size_t response_bytes,
                   std::uint64_t duration_ms);

    static bool setNonBlocking(int fd);
    static bool sendAll(int fd, const std::string& data);

    std::uint16_t port_ = 0;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    std::uint64_t next_generation_ = 1;
    std::unordered_map<int, ClientState> clients_;
    std::mutex responses_mutex_;
    std::queue<PendingResponse> responses_;
    Logger& logger_;
    Metrics metrics_;
    std::uint64_t slow_request_ms_ = 200;
    std::string auth_token_;
    std::atomic<std::uint64_t> next_request_id_ {1};
    std::size_t max_connections_ = 1024;
    std::filesystem::path upload_tmp_dir_;
    std::size_t max_request_bytes_ = 10 * 1024 * 1024;
    std::size_t max_upload_bytes_ = 128 * 1024 * 1024;
    std::size_t stream_upload_threshold_bytes_ = 1024 * 1024;
    std::uint64_t request_timeout_ms_ = 5000;
    std::uint64_t upload_timeout_ms_ = 30000;
    ObjectStore object_store_;
    AuthService auth_service_;
    Router router_;
    ThreadPool thread_pool_;
};

} // namespace mini_oss
