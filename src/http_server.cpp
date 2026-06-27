#include "mini_oss/http_server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <queue>
#include <sstream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mini_oss {
namespace {

constexpr int kBacklog = 128;
constexpr int kMaxEvents = 1024;
constexpr std::uint64_t kWakeupValue = 1;
constexpr int kEpollWaitMs = 100;
constexpr int kBufferSize = 4096;
constexpr std::size_t kMaxHeaderSize = 16 * 1024;
constexpr std::size_t kDefaultMaxRequestSize = 10 * 1024 * 1024;
constexpr std::size_t kDefaultMaxUploadSize = 128 * 1024 * 1024;
constexpr std::size_t kDefaultStreamUploadThreshold = 1024 * 1024;
constexpr std::size_t kDefaultMaxConnections = 1024;
constexpr std::size_t kFileSendChunk = 256 * 1024;

std::string socketError(const char* operation)
{
    return std::string(operation) + " failed: " + std::strerror(errno);
}

std::uint64_t elapsedMs(std::chrono::steady_clock::time_point started_at)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
}

std::optional<std::uint64_t> parseContentLength(const HttpRequest& request, std::string& error)
{
    const auto it = request.headers.find("content-length");
    if (it == request.headers.end()) {
        return 0;
    }

    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(it->second.c_str(), &end, 10);
    if (errno != 0 || end == it->second.c_str() || *end != '\0') {
        error = "invalid Content-Length";
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(parsed);
}

void cleanupTemporaryRequestBody(const HttpRequest& request)
{
    if (request.temporary_body_file && !request.body_file_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(request.body_file_path, ignored);
    }
}

std::string pathWithoutQuery(const std::string& path)
{
    const auto query_pos = path.find('?');
    return query_pos == std::string::npos ? path : path.substr(0, query_pos);
}

std::map<std::string, std::string> queryParams(const std::string& path)
{
    std::map<std::string, std::string> params;
    const auto query_pos = path.find('?');
    if (query_pos == std::string::npos || query_pos + 1 >= path.size()) {
        return params;
    }

    std::size_t pos = query_pos + 1;
    while (pos < path.size()) {
        const auto amp = path.find('&', pos);
        const auto end = amp == std::string::npos ? path.size() : amp;
        const auto equals = path.find('=', pos);
        if (equals != std::string::npos && equals < end) {
            params[path.substr(pos, equals - pos)] = path.substr(equals + 1, end - equals - 1);
        } else if (end > pos) {
            params[path.substr(pos, end - pos)] = "";
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return params;
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream oss;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            oss << ch;
            break;
        }
    }
    return oss.str();
}

std::string nowIso()
{
    const auto current = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(current);
    std::tm tm {};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::size_t parseSizeOrDefault(const std::string& value, std::size_t fallback)
{
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

std::string jsonStringValue(const std::string& body, const std::string& key)
{
    const std::string quoted_key = "\"" + key + "\"";
    const auto key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return {};
    }
    const auto colon = body.find(':', key_pos + quoted_key.size());
    if (colon == std::string::npos) {
        return {};
    }
    const auto first_quote = body.find('"', colon + 1);
    if (first_quote == std::string::npos) {
        return {};
    }
    const auto second_quote = body.find('"', first_quote + 1);
    if (second_quote == std::string::npos) {
        return {};
    }
    return body.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::string rolesJson(const std::vector<std::string>& roles)
{
    std::ostringstream oss;
    oss << '[';
    bool first = true;
    for (const auto& role : roles) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << "\"" << jsonEscape(role) << "\"";
    }
    oss << ']';
    return oss.str();
}

std::string statusCodesJson(const std::map<int, std::uint64_t>& status_codes)
{
    std::ostringstream oss;
    oss << '{';
    bool first = true;
    for (const auto& item : status_codes) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << "\"" << item.first << "\":" << item.second;
    }
    oss << '}';
    return oss.str();
}

std::string extractObjectIdFromPath(const std::string& request_path)
{
    const std::string path = pathWithoutQuery(request_path);
    constexpr const char* prefix = "/objects/";
    if (path.rfind(prefix, 0) != 0 || path.size() <= std::string(prefix).size()) {
        return {};
    }
    const auto id = path.substr(std::string(prefix).size());
    return id.find('/') == std::string::npos ? id : std::string();
}

std::string actionForRequest(const HttpRequest& request)
{
    const std::string path = pathWithoutQuery(request.path);
    if (path == "/auth/login") {
        return "login";
    }
    if (path == "/objects" && request.method == HttpMethod::Post) {
        return "upload_object";
    }
    if (path == "/objects/instant" && request.method == HttpMethod::Post) {
        return "instant_upload";
    }
    if (path == "/objects" && request.method == HttpMethod::Get) {
        return "list_objects";
    }
    if (path.rfind("/objects/", 0) == 0 && request.method == HttpMethod::Get) {
        return "download_object";
    }
    if (path.rfind("/objects/", 0) == 0 && request.method == HttpMethod::Delete) {
        return "delete_object";
    }
    if (path == "/admin/users" && request.method == HttpMethod::Get) {
        return "list_users";
    }
    if (path.rfind("/admin/users/", 0) == 0 && request.method == HttpMethod::Put) {
        return "update_user_roles";
    }
    if (path == "/admin/audit-logs") {
        return "query_audit_logs";
    }
    if (path.rfind("/admin/stats/", 0) == 0) {
        return "query_admin_stats";
    }
    return {};
}

} // namespace

HttpServer::HttpServer(std::uint16_t port, std::size_t worker_threads,
                       std::filesystem::path storage_dir, Logger& logger,
                       std::uint64_t slow_request_ms, std::string auth_token,
                       std::size_t max_request_bytes, std::size_t max_upload_bytes,
                       std::size_t stream_upload_threshold_bytes,
                       std::size_t max_connections, std::size_t thread_queue_limit,
                       std::uint64_t request_timeout_ms, std::uint64_t upload_timeout_ms,
                       RedisConfig redis_config)
    : port_(port)
    , logger_(logger)
    , slow_request_ms_(slow_request_ms)
    , auth_token_(std::move(auth_token))
    , max_connections_(max_connections == 0 ? kDefaultMaxConnections : max_connections)
    , upload_tmp_dir_(storage_dir / "tmp_uploads")
    , max_request_bytes_(max_request_bytes == 0 ? kDefaultMaxRequestSize : max_request_bytes)
    , max_upload_bytes_(max_upload_bytes == 0 ? kDefaultMaxUploadSize : max_upload_bytes)
    , stream_upload_threshold_bytes_(
          stream_upload_threshold_bytes == 0 ? 0 : stream_upload_threshold_bytes)
    , request_timeout_ms_(request_timeout_ms)
    , upload_timeout_ms_(upload_timeout_ms)
    , object_store_(std::move(storage_dir), std::move(redis_config), &metrics_)
    , auth_service_(object_store_.metadataStore())
    , thread_pool_(worker_threads, thread_queue_limit)
{
    if (stream_upload_threshold_bytes_ > max_upload_bytes_) {
        stream_upload_threshold_bytes_ = max_upload_bytes_;
    }

    std::string auth_error;
    if (!auth_service_.initializeDefaults(auth_error)) {
        logger_.error("cannot initialize default users and roles: " + auth_error);
    }

    router_.addRoute(HttpMethod::Post, "/auth/login", [this](const HttpRequest& request) {
        return auth_service_.login(request);
    });
    router_.addRoute(HttpMethod::Get, "/admin/users", [this](const HttpRequest& request) {
        return handleAdminUsers(request);
    });
    router_.addPrefixRoute(HttpMethod::Put, "/admin/users/", [this](const HttpRequest& request) {
        return handleUpdateUserRoles(request);
    });
    router_.addRoute(HttpMethod::Get, "/admin/audit-logs", [this](const HttpRequest& request) {
        return handleAuditLogs(request);
    });
    router_.addRoute(HttpMethod::Get, "/admin/stats/overview", [this](const HttpRequest& request) {
        return handleAdminStatsOverview(request);
    });
    router_.addRoute(HttpMethod::Get, "/admin/stats/status-codes", [this](const HttpRequest& request) {
        return handleAdminStatsStatusCodes(request);
    });
    router_.addRoute(HttpMethod::Get, "/admin/stats/redis", [this](const HttpRequest& request) {
        return handleAdminStatsRedis(request);
    });

    router_.addRoute(HttpMethod::Get, "/health", [](const HttpRequest&) {
        return HttpResponse::json(200, "OK", "{\"status\":\"ok\"}\n");
    });
    router_.addRoute(HttpMethod::Get, "/metrics", [this](const HttpRequest&) {
        std::string body = metrics_.toJson();
        if (!body.empty() && body.back() == '}') {
            body.pop_back();
            body += ",\"log_dropped_entries\":" + std::to_string(logger_.droppedCount()) + "}";
        }
        return HttpResponse::json(200, "OK", body + "\n");
    });
    router_.addRoute(HttpMethod::Post, "/objects", [this](const HttpRequest& request) {
        return object_store_.createObject(request);
    });
    router_.addRoute(HttpMethod::Post, "/objects/instant", [this](const HttpRequest& request) {
        return object_store_.createInstantObject(request);
    });
    router_.addRoute(HttpMethod::Get, "/objects", [this](const HttpRequest& request) {
        return object_store_.listObjects(request);
    });
    router_.addPrefixRoute(HttpMethod::Get, "/objects/", [this](const HttpRequest& request) {
        return object_store_.getObject(request);
    });
    router_.addPrefixRoute(HttpMethod::Delete, "/objects/", [this](const HttpRequest& request) {
        return object_store_.deleteObject(request);
    });
}

HttpServer::~HttpServer()
{
    thread_pool_.shutdown();
    stop();
}

bool HttpServer::start()
{
    std::error_code ec;
    std::filesystem::create_directories(upload_tmp_dir_, ec);
    if (ec) {
        logServerError("cannot create upload temp directory: " + ec.message());
        return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        logServerError(socketError("socket"));
        return false;
    }

    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        logServerError(socketError("setsockopt"));
        return false;
    }

    if (!setNonBlocking(listen_fd_)) {
        return false;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logServerError(socketError("bind"));
        return false;
    }

    if (::listen(listen_fd_, kBacklog) < 0) {
        logServerError(socketError("listen"));
        return false;
    }

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        logServerError(socketError("epoll_create1"));
        return false;
    }

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        logServerError(socketError("eventfd"));
        return false;
    }

    epoll_event wake_event {};
    wake_event.events = EPOLLIN;
    wake_event.data.fd = wake_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &wake_event) < 0) {
        logServerError(socketError("epoll_ctl wake fd"));
        return false;
    }

    epoll_event event {};
    event.events = EPOLLIN;
    event.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        logServerError(socketError("epoll_ctl listen fd"));
        return false;
    }

    std::cout << "Mini-OSS HTTP server listening on 0.0.0.0:" << port_ << '\n';
    std::cout << "Worker threads: " << thread_pool_.threadCount() << '\n';
    logger_.info("server listening on port " + std::to_string(port_));
    return true;
}

void HttpServer::run(const std::function<bool()>& keep_running)
{
    epoll_event events[kMaxEvents];

    while (keep_running()) {
        const int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, kEpollWaitMs);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            logServerError(socketError("epoll_wait"));
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const auto event_mask = events[i].events;

            if (fd == listen_fd_) {
                acceptClients();
                continue;
            }

            if (fd == wake_fd_) {
                handleWakeup();
                continue;
            }

            if ((event_mask & EPOLLOUT) != 0) {
                handleClientWrite(fd);
                continue;
            }

            if ((event_mask & EPOLLIN) != 0) {
                handleClientRead(fd);
                continue;
            }

            if ((event_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                closeClient(fd);
            }
        }

        closeTimedOutClients();
    }

    sendCompletedResponses();
}

void HttpServer::stop()
{
    for (auto& item : clients_) {
        cleanupClientUpload(item.second);
        ::close(item.first);
    }
    clients_.clear();

    {
        std::lock_guard<std::mutex> lock(responses_mutex_);
        std::queue<PendingResponse> empty;
        responses_.swap(empty);
    }

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

void HttpServer::acceptClients()
{
    while (true) {
        sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr),
                                        &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            logServerError(socketError("accept"));
            break;
        }

        char addr_text[INET_ADDRSTRLEN] {};
        const char* remote = ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_text, sizeof(addr_text));
        std::ostringstream remote_addr;
        remote_addr << (remote == nullptr ? "unknown" : remote) << ':' << ntohs(client_addr.sin_port);
        const std::string remote_string = remote_addr.str();

        if (max_connections_ > 0 && clients_.size() >= max_connections_) {
            metrics_.connectionRejected();
            const auto response = HttpResponse::text(503, "Service Unavailable",
                                                     "too many connections\n")
                                      .serialize();
            sendAll(client_fd, response);
            logAccess(remote_string, "-", "-", 503, 0, response.size(), 0);
            ::close(client_fd);
            continue;
        }

        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            logServerError(socketError("epoll_ctl client fd"));
            ::close(client_fd);
            continue;
        }

        ClientState state;
        const auto now = std::chrono::steady_clock::now();
        state.remote_addr = remote_string;
        state.started_at = now;
        state.last_activity_at = now;
        state.generation = next_generation_++;
        clients_.emplace(client_fd, std::move(state));
        metrics_.connectionOpened();
    }
}

void HttpServer::handleWakeup()
{
    std::uint64_t value = 0;
    while (::read(wake_fd_, &value, sizeof(value)) > 0) {
    }
    sendCompletedResponses();
}

void HttpServer::handleClientRead(int client_fd)
{
    const auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end() || client_it->second.processing) {
        return;
    }

    char buffer[kBufferSize];
    auto& state = client_it->second;

    while (true) {
        const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            state.last_activity_at = std::chrono::steady_clock::now();
            if (state.streaming_upload) {
                if (!writeStreamingUpload(state, buffer, static_cast<std::size_t>(n))) {
                    sendImmediateResponse(client_fd,
                                          HttpResponse::text(500, "Internal Server Error",
                                                             "cannot write upload temp file\n"),
                                          httpMethodName(state.header_request.method),
                                          state.header_request.path, state.request_bytes);
                    return;
                }
                if (state.received_body_bytes >= state.content_length) {
                    submitStreamingUpload(client_fd, state);
                    return;
                }
                continue;
            }

            state.buffer.append(buffer, static_cast<std::size_t>(n));
            const auto header_end = state.buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (state.buffer.size() > kMaxHeaderSize) {
                    sendImmediateResponse(client_fd,
                                          HttpResponse::text(431, "Request Header Fields Too Large",
                                                             "request header too large\n"),
                                          "-", "-", state.buffer.size());
                    return;
                }
                if (state.buffer.size() > max_request_bytes_) {
                    sendImmediateResponse(client_fd,
                                          HttpResponse::text(413, "Payload Too Large",
                                                             "request body too large\n"),
                                          "-", "-", state.buffer.size());
                    return;
                }
                continue;
            }

            const std::size_t body_begin = header_end + 4;
            if (!state.header_parsed) {
                const auto head_result = parseHttpRequestHead(state.buffer.substr(0, body_begin));
                if (!head_result.complete || !head_result.ok) {
                    const std::string error = head_result.error.empty() ? "invalid request" : head_result.error;
                    sendImmediateResponse(client_fd, HttpResponse::badRequest(error), "-", "-",
                                          state.buffer.size());
                    return;
                }

                std::string length_error;
                const auto content_length = parseContentLength(head_result.request, length_error);
                if (!content_length.has_value()) {
                    sendImmediateResponse(client_fd, HttpResponse::badRequest(length_error),
                                          httpMethodName(head_result.request.method),
                                          head_result.request.path, state.buffer.size());
                    return;
                }

                if (shouldStreamUpload(head_result.request, content_length.value())) {
                    HttpRequest stream_request = head_result.request;
                    const auto auth_response = authorizeRequest(stream_request);
                    if (auth_response.has_value()) {
                        sendImmediateResponse(client_fd, auth_response.value(),
                                              httpMethodName(stream_request.method),
                                              stream_request.path, body_begin);
                        return;
                    }
                    if (content_length.value() > max_upload_bytes_) {
                        sendImmediateResponse(client_fd,
                                              HttpResponse::text(413, "Payload Too Large",
                                                                 "upload body too large\n"),
                                              httpMethodName(head_result.request.method),
                                              head_result.request.path, body_begin);
                        return;
                    }

                    const std::uint64_t request_bytes = static_cast<std::uint64_t>(body_begin)
                        + content_length.value();
                    if (request_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                        sendImmediateResponse(client_fd,
                                              HttpResponse::text(413, "Payload Too Large",
                                                                 "upload body too large\n"),
                                              httpMethodName(head_result.request.method),
                                              head_result.request.path, body_begin);
                        return;
                    }

                    if (!beginStreamingUpload(client_fd, state, std::move(stream_request),
                                              content_length.value(),
                                              static_cast<std::size_t>(request_bytes))) {
                        sendImmediateResponse(client_fd,
                                              HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot create upload temp file\n"),
                                              httpMethodName(head_result.request.method),
                                              head_result.request.path, body_begin);
                        return;
                    }

                    const auto available = state.buffer.size() > body_begin
                        ? state.buffer.size() - body_begin
                        : 0;
                    const auto to_write = static_cast<std::size_t>(
                        std::min<std::uint64_t>(available, state.content_length));
                    if (!writeStreamingUpload(state, state.buffer.data() + body_begin, to_write)) {
                        sendImmediateResponse(client_fd,
                                              HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot write upload temp file\n"),
                                              httpMethodName(state.header_request.method),
                                              state.header_request.path, state.request_bytes);
                        return;
                    }
                    state.buffer.clear();
                    if (state.received_body_bytes >= state.content_length) {
                        submitStreamingUpload(client_fd, state);
                        return;
                    }
                    continue;
                }

                const std::uint64_t request_bytes = static_cast<std::uint64_t>(body_begin)
                    + content_length.value();
                if (request_bytes > max_request_bytes_) {
                    sendImmediateResponse(client_fd,
                                          HttpResponse::text(413, "Payload Too Large",
                                                             "request body too large\n"),
                                          httpMethodName(head_result.request.method),
                                          head_result.request.path, body_begin);
                    return;
                }
                if (request_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    sendImmediateResponse(client_fd,
                                          HttpResponse::text(413, "Payload Too Large",
                                                             "request body too large\n"),
                                          httpMethodName(head_result.request.method),
                                          head_result.request.path, body_begin);
                    return;
                }

                state.header_request = head_result.request;
                state.header_parsed = true;
                state.content_length = content_length.value();
                state.request_bytes = static_cast<std::size_t>(request_bytes);
            }

            if (state.buffer.size() > max_request_bytes_) {
                sendImmediateResponse(client_fd,
                                      HttpResponse::text(413, "Payload Too Large",
                                                         "request body too large\n"),
                                      "-", "-", state.buffer.size());
                return;
            }
            if (state.buffer.size() >= state.request_bytes) {
                auto parse_result = parseHttpRequest(state.buffer);
                if (!parse_result.complete || !parse_result.ok) {
                    const std::string error = parse_result.error.empty() ? "invalid request" : parse_result.error;
                    sendImmediateResponse(client_fd, HttpResponse::badRequest(error), "-", "-",
                                          state.buffer.size());
                    return;
                }

                const std::uint64_t generation = state.generation;
                const std::string remote_addr = state.remote_addr;
                const auto started_at = state.started_at;
                const auto request_bytes = state.request_bytes;
                state.processing = true;

                epoll_event event {};
                event.events = EPOLLRDHUP;
                event.data.fd = client_fd;
                if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event) < 0) {
                    logServerError(socketError("epoll_ctl disable read"));
                    closeClient(client_fd);
                    return;
                }

                submitRequest(client_fd, generation, remote_addr, started_at,
                              std::move(parse_result.request), request_bytes);
                return;
            }
            continue;
        }

        if (n == 0) {
            closeClient(client_fd);
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        logServerError(socketError("recv"));
        closeClient(client_fd);
        return;
    }
}

void HttpServer::submitRequest(int client_fd, std::uint64_t generation, std::string remote_addr,
                               std::chrono::steady_clock::time_point started_at,
                               HttpRequest request, std::size_t request_bytes)
{
    const bool queued = thread_pool_.enqueue([this, client_fd, generation, remote_addr = std::move(remote_addr),
                                              started_at, request = std::move(request), request_bytes]() mutable {
        HttpResponse http_response = HttpResponse::badRequest("invalid request");
        const std::string method = httpMethodName(request.method);
        const std::string path = request.path;
        const bool object_upload = request.method == HttpMethod::Post && request.path == "/objects";
        const bool streamed_upload = request.body_in_file;
        const std::size_t upload_body_size = request.body_in_file
            ? static_cast<std::size_t>(request.body_size)
            : request.body.size();

        request.request_id = std::to_string(next_request_id_.fetch_add(1, std::memory_order_relaxed));
        const auto auth_response = authorizeRequest(request);
        if (auth_response.has_value()) {
            http_response = auth_response.value();
        } else {
            http_response = router_.route(request);
            if (object_upload && http_response.statusCode() >= 200
                && http_response.statusCode() < 300) {
                metrics_.recordObjectUpload(upload_body_size, streamed_upload);
            }
        }
        recordAudit(request, http_response, remote_addr, started_at);
        cleanupTemporaryRequestBody(request);

        enqueueResponse(client_fd, generation, std::move(http_response), remote_addr, method, path,
                        started_at, request_bytes);
    });

    if (!queued) {
        metrics_.queueRejected();
        cleanupTemporaryRequestBody(request);
        enqueueResponse(client_fd, generation,
                        HttpResponse::text(503, "Service Unavailable", "server busy\n"),
                        remote_addr, httpMethodName(request.method), request.path, started_at,
                        request_bytes);
    }
}

void HttpServer::enqueueResponse(int client_fd, std::uint64_t generation, HttpResponse response,
                                 std::string remote_addr, std::string method, std::string path,
                                 std::chrono::steady_clock::time_point started_at,
                                 std::size_t request_bytes)
{
    {
        std::lock_guard<std::mutex> lock(responses_mutex_);
        responses_.push(PendingResponse {client_fd, generation, std::move(response),
                                         std::move(remote_addr), std::move(method),
                                         std::move(path), started_at, request_bytes});
    }

    if (wake_fd_ >= 0) {
        const std::uint64_t value = kWakeupValue;
        const ssize_t ignored = ::write(wake_fd_, &value, sizeof(value));
        (void)ignored;
    }
}

void HttpServer::sendCompletedResponses()
{
    std::queue<PendingResponse> responses;
    {
        std::lock_guard<std::mutex> lock(responses_mutex_);
        responses.swap(responses_);
    }

    while (!responses.empty()) {
        auto response = std::move(responses.front());
        responses.pop();

        const auto client_it = clients_.find(response.client_fd);
        if (client_it == clients_.end() || client_it->second.generation != response.generation) {
            continue;
        }

        beginResponse(std::move(response));
    }
}

void HttpServer::beginResponse(PendingResponse response)
{
    const auto client_it = clients_.find(response.client_fd);
    if (client_it == clients_.end() || client_it->second.generation != response.generation) {
        return;
    }

    auto& state = client_it->second;
    state.response_buffer = response.response.bodyInFile()
        ? response.response.serializeHeaders()
        : response.response.serialize();
    state.response_buffer_sent = 0;
    state.response_file = response.response.fileBody();
    state.response_file_offset = state.response_file == nullptr ? 0 : state.response_file->offset;
    state.response_file_remaining = state.response_file == nullptr ? 0 : state.response_file->length;
    state.response_total_bytes = state.response_buffer.size()
        + static_cast<std::size_t>(state.response_file_remaining);
    state.response_status_code = response.response.statusCode();
    state.response_method = std::move(response.method);
    state.response_path = std::move(response.path);
    state.response_remote_addr = std::move(response.remote_addr);
    state.response_request_bytes = response.request_bytes;
    state.response_started_at = response.started_at;
    state.processing = false;
    state.writing_response = true;
    state.last_activity_at = std::chrono::steady_clock::now();

    epoll_event event {};
    event.events = EPOLLOUT | EPOLLRDHUP;
    event.data.fd = response.client_fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, response.client_fd, &event) < 0) {
        logServerError(socketError("epoll_ctl enable write"));
        closeClient(response.client_fd);
        return;
    }

    handleClientWrite(response.client_fd);
}

void HttpServer::handleClientWrite(int client_fd)
{
    const auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end() || !client_it->second.writing_response) {
        return;
    }

    auto& state = client_it->second;
    while (state.response_buffer_sent < state.response_buffer.size()) {
        const auto* data = state.response_buffer.data() + state.response_buffer_sent;
        const auto remaining = state.response_buffer.size() - state.response_buffer_sent;
        const ssize_t n = ::send(client_fd, data, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            state.response_buffer_sent += static_cast<std::size_t>(n);
            state.last_activity_at = std::chrono::steady_clock::now();
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        closeClient(client_fd);
        return;
    }

    while (state.response_file != nullptr && state.response_file_remaining > 0) {
        off_t offset = static_cast<off_t>(state.response_file_offset);
        const auto chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(state.response_file_remaining, kFileSendChunk));
        const ssize_t n = ::sendfile(client_fd, state.response_file->fd, &offset, chunk);
        if (n > 0) {
            state.response_file_offset = static_cast<std::uint64_t>(offset);
            state.response_file_remaining -= static_cast<std::uint64_t>(n);
            state.last_activity_at = std::chrono::steady_clock::now();
            continue;
        }
        if (n == 0) {
            closeClient(client_fd);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        closeClient(client_fd);
        return;
    }

    finishResponse(client_fd);
}

void HttpServer::finishResponse(int client_fd)
{
    const auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end()) {
        return;
    }

    auto& state = client_it->second;
    if (state.response_file != nullptr) {
        const auto file_length = state.response_file->length;
        if (file_length <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            metrics_.recordFileDownload(static_cast<std::size_t>(file_length));
        }
    }
    logAccess(state.response_remote_addr, state.response_method, state.response_path,
              state.response_status_code, state.response_request_bytes,
              state.response_total_bytes, elapsedMs(state.response_started_at));
    closeClient(client_fd);
}

void HttpServer::closeClient(int client_fd)
{
    if (epoll_fd_ >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    }
    const auto it = clients_.find(client_fd);
    if (it != clients_.end()) {
        cleanupClientUpload(it->second);
        clients_.erase(it);
        metrics_.connectionClosed();
    }
    ::close(client_fd);
}

void HttpServer::closeTimedOutClients()
{
    if (clients_.empty() || (request_timeout_ms_ == 0 && upload_timeout_ms_ == 0)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<int> timed_out_clients;
    timed_out_clients.reserve(clients_.size());

    for (const auto& item : clients_) {
        const auto& state = item.second;
        if (state.processing) {
            continue;
        }

        const auto timeout_ms = state.streaming_upload ? upload_timeout_ms_ : request_timeout_ms_;
        if (timeout_ms == 0) {
            continue;
        }

        const auto idle_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - state.last_activity_at)
                .count());
        if (idle_ms >= timeout_ms) {
            timed_out_clients.push_back(item.first);
        }
    }

    for (int client_fd : timed_out_clients) {
        const auto client_it = clients_.find(client_fd);
        if (client_it == clients_.end()) {
            continue;
        }
        const auto& state = client_it->second;
        if (state.writing_response) {
            metrics_.requestTimedOut();
            closeClient(client_fd);
            continue;
        }
        if (state.streaming_upload) {
            metrics_.uploadTimedOut();
        } else {
            metrics_.requestTimedOut();
        }
        const std::string method = state.header_parsed ? httpMethodName(state.header_request.method) : "-";
        const std::string path = state.header_parsed ? state.header_request.path : "-";
        const auto request_bytes = state.request_bytes == 0 ? state.buffer.size() : state.request_bytes;
        sendImmediateResponse(client_fd, HttpResponse::text(408, "Request Timeout",
                                                            "request timeout\n"),
                              method, path, request_bytes);
    }
}

void HttpServer::logServerError(const std::string& message)
{
    std::cerr << message << '\n';
    logger_.error(message);
}

void HttpServer::sendImmediateResponse(int client_fd, const HttpResponse& response,
                                       const std::string& method, const std::string& path,
                                       std::size_t request_bytes)
{
    const auto client_it = clients_.find(client_fd);
    if (client_it == clients_.end()) {
        return;
    }

    const auto serialized = response.serialize();
    const auto duration_ms = elapsedMs(client_it->second.started_at);
    sendAll(client_fd, serialized);
    logAccess(client_it->second.remote_addr, method, path, response.statusCode(), request_bytes,
              serialized.size(), duration_ms);
    closeClient(client_fd);
}

bool HttpServer::shouldStreamUpload(const HttpRequest& request, std::uint64_t content_length) const
{
    return request.method == HttpMethod::Post && request.path == "/objects"
        && content_length > stream_upload_threshold_bytes_;
}

bool HttpServer::beginStreamingUpload(int client_fd, ClientState& state, HttpRequest request,
                                      std::uint64_t content_length, std::size_t request_bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(upload_tmp_dir_, ec);
    if (ec) {
        logServerError("cannot create upload temp directory: " + ec.message());
        return false;
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto filename = "upload_" + std::to_string(now) + "_"
            + std::to_string(client_fd) + "_" + std::to_string(attempt) + ".tmp";
        const auto path = upload_tmp_dir_ / filename;
        const int fd = ::open(path.string().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            state.header_parsed = true;
            state.streaming_upload = true;
            state.content_length = content_length;
            state.received_body_bytes = 0;
            state.request_bytes = request_bytes;
            state.header_request = std::move(request);
            state.temp_upload_path = path;
            state.temp_upload_fd = fd;
            state.owns_temp_upload = true;
            return true;
        }
        if (errno != EEXIST) {
            logServerError(socketError("open upload temp file"));
            return false;
        }
    }

    logServerError("cannot allocate unique upload temp file");
    return false;
}

bool HttpServer::writeStreamingUpload(ClientState& state, const char* data, std::size_t size)
{
    if (size == 0) {
        return true;
    }
    if (state.temp_upload_fd < 0 || state.received_body_bytes >= state.content_length) {
        return true;
    }

    const auto remaining = state.content_length - state.received_body_bytes;
    std::size_t to_write = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(size)));
    const char* cursor = data;
    while (to_write > 0) {
        const ssize_t n = ::write(state.temp_upload_fd, cursor, to_write);
        if (n > 0) {
            cursor += n;
            to_write -= static_cast<std::size_t>(n);
            state.received_body_bytes += static_cast<std::uint64_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        logServerError(socketError("write upload temp file"));
        return false;
    }
    return true;
}

void HttpServer::cleanupClientUpload(ClientState& state)
{
    if (state.temp_upload_fd >= 0) {
        ::close(state.temp_upload_fd);
        state.temp_upload_fd = -1;
    }
    if (state.owns_temp_upload && !state.temp_upload_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(state.temp_upload_path, ignored);
    }
    state.owns_temp_upload = false;
}

void HttpServer::submitStreamingUpload(int client_fd, ClientState& state)
{
    if (state.temp_upload_fd >= 0) {
        ::close(state.temp_upload_fd);
        state.temp_upload_fd = -1;
    }

    HttpRequest request = std::move(state.header_request);
    request.body_in_file = true;
    request.temporary_body_file = true;
    request.body_file_path = state.temp_upload_path;
    request.body_size = state.received_body_bytes;

    const std::uint64_t generation = state.generation;
    const std::string remote_addr = state.remote_addr;
    const auto started_at = state.started_at;
    const auto request_bytes = state.request_bytes;
    state.processing = true;
    state.owns_temp_upload = false;

    epoll_event event {};
    event.events = EPOLLRDHUP;
    event.data.fd = client_fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event) < 0) {
        cleanupTemporaryRequestBody(request);
        logServerError(socketError("epoll_ctl disable read"));
        closeClient(client_fd);
        return;
    }

    submitRequest(client_fd, generation, remote_addr, started_at, std::move(request), request_bytes);
}

bool HttpServer::requiresAuth(const HttpRequest& request) const
{
    const std::string path = pathWithoutQuery(request.path);
    return path == "/objects" || path.rfind("/objects/", 0) == 0
        || path.rfind("/admin/", 0) == 0;
}

std::optional<HttpResponse> HttpServer::authorizeRequest(HttpRequest& request)
{
    if (!requiresAuth(request)) {
        return std::nullopt;
    }

    const std::string path = pathWithoutQuery(request.path);
    std::string error;
    if (auth_service_.authenticate(request, error)) {
        if (path.rfind("/admin/", 0) == 0 && !request.is_admin) {
            return HttpResponse::forbidden();
        }
        return std::nullopt;
    }

    if (path.rfind("/admin/", 0) == 0) {
        return HttpResponse::unauthorized();
    }

    if (isLegacyTokenAuthorized(request)) {
        request.authenticated = true;
        request.is_admin = true;
        request.legacy_token = true;
        request.username = "legacy-token";
        request.roles = {"admin"};
        return std::nullopt;
    }
    return HttpResponse::unauthorized();
}

bool HttpServer::isLegacyTokenAuthorized(const HttpRequest& request) const
{
    if (auth_token_.empty()) {
        return false;
    }
    const auto token_it = request.headers.find("x-auth-token");
    if (token_it != request.headers.end() && token_it->second == auth_token_) {
        return true;
    }

    const auto authorization_it = request.headers.find("authorization");
    if (authorization_it == request.headers.end()) {
        return false;
    }

    const std::string bearer_prefix = "Bearer ";
    return authorization_it->second == bearer_prefix + auth_token_;
}

HttpResponse HttpServer::handleAdminUsers(const HttpRequest&)
{
    std::string error;
    const auto users = object_store_.metadataStore().listUsers(error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot list users: " + error + "\n");
    }

    std::ostringstream body;
    body << "{\"users\":[";
    bool first = true;
    for (const auto& user : users) {
        if (!first) {
            body << ',';
        }
        first = false;
        body << "{"
             << "\"id\":" << user.id << ','
             << "\"username\":\"" << jsonEscape(user.username) << "\","
             << "\"status\":" << user.status << ','
             << "\"roles\":" << rolesJson(user.roles) << ','
             << "\"created_at\":\"" << jsonEscape(user.created_at) << "\","
             << "\"updated_at\":\"" << jsonEscape(user.updated_at) << "\","
             << "\"last_login_at\":\"" << jsonEscape(user.last_login_at) << "\""
             << "}";
    }
    body << "]}\n";
    return HttpResponse::json(200, "OK", body.str());
}

HttpResponse HttpServer::handleUpdateUserRoles(const HttpRequest& request)
{
    const std::string path = pathWithoutQuery(request.path);
    constexpr const char* prefix = "/admin/users/";
    constexpr const char* suffix = "/roles";
    if (path.rfind(prefix, 0) != 0 || path.size() <= std::string(prefix).size()
        || path.size() < std::string(suffix).size()
        || path.substr(path.size() - std::string(suffix).size()) != suffix) {
        return HttpResponse::badRequest("invalid user role path");
    }

    const auto id_text = path.substr(std::string(prefix).size(),
                                     path.size() - std::string(prefix).size()
                                         - std::string(suffix).size());
    const auto user_id = parseSizeOrDefault(id_text, 0);
    if (user_id == 0) {
        return HttpResponse::badRequest("invalid user id");
    }

    std::string role = jsonStringValue(request.body, "role");
    if (role.empty()) {
        const auto role_header = request.headers.find("x-role");
        if (role_header != request.headers.end()) {
            role = role_header->second;
        }
    }
    if (role != "admin" && role != "user") {
        return HttpResponse::badRequest("role must be admin or user");
    }

    const std::vector<std::string> roles = role == "admin"
        ? std::vector<std::string> {"admin"}
        : std::vector<std::string> {"user"};
    std::string error;
    if (!object_store_.metadataStore().replaceUserRoles(static_cast<int>(user_id), roles, error)) {
        return HttpResponse::text(500, "Internal Server Error", "cannot update user roles: " + error + "\n");
    }
    return HttpResponse::json(200, "OK",
                              std::string("{\"updated\":true,\"user_id\":")
                                  + std::to_string(user_id) + ",\"roles\":" + rolesJson(roles) + "}\n");
}

HttpResponse HttpServer::handleAuditLogs(const HttpRequest& request)
{
    const auto params = queryParams(request.path);
    AuditLogQuery query;
    auto it = params.find("limit");
    if (it != params.end()) {
        query.limit = std::min<std::size_t>(parseSizeOrDefault(it->second, query.limit), 500);
    }
    it = params.find("offset");
    if (it != params.end()) {
        query.offset = parseSizeOrDefault(it->second, 0);
    }
    it = params.find("user_id");
    if (it != params.end()) {
        query.user_id = static_cast<int>(parseSizeOrDefault(it->second, 0));
    }
    it = params.find("action");
    if (it != params.end()) {
        query.action = it->second;
    }

    std::string error;
    const auto records = object_store_.metadataStore().listAuditLogs(query, error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot list audit logs: " + error + "\n");
    }

    std::ostringstream body;
    body << "{\"audit_logs\":[";
    bool first = true;
    for (const auto& record : records) {
        if (!first) {
            body << ',';
        }
        first = false;
        body << "{"
             << "\"id\":" << record.id << ','
             << "\"request_id\":\"" << jsonEscape(record.request_id) << "\","
             << "\"user_id\":" << record.user_id << ','
             << "\"username\":\"" << jsonEscape(record.username) << "\","
             << "\"method\":\"" << jsonEscape(record.method) << "\","
             << "\"path\":\"" << jsonEscape(record.path) << "\","
             << "\"action\":\"" << jsonEscape(record.action) << "\","
             << "\"file_id\":\"" << jsonEscape(record.file_id) << "\","
             << "\"status_code\":" << record.status_code << ','
             << "\"result\":\"" << jsonEscape(record.result) << "\","
             << "\"error_message\":\"" << jsonEscape(record.error_message) << "\","
             << "\"client_ip\":\"" << jsonEscape(record.client_ip) << "\","
             << "\"latency_ms\":" << record.latency_ms << ','
             << "\"created_at\":\"" << jsonEscape(record.created_at) << "\""
             << "}";
    }
    body << "]}\n";
    return HttpResponse::json(200, "OK", body.str());
}

HttpResponse HttpServer::handleAdminStatsOverview(const HttpRequest&)
{
    std::string error;
    const auto storage = object_store_.metadataStore().objectStorageStats(error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot load storage stats: " + error + "\n");
    }
    const auto metrics = metrics_.snapshot();
    const auto redis_total = metrics.metadata_cache_hits + metrics.metadata_cache_misses;
    const double redis_hit_rate = redis_total == 0
        ? 0.0
        : static_cast<double>(metrics.metadata_cache_hits) / static_cast<double>(redis_total);

    std::ostringstream body;
    body << std::fixed << std::setprecision(3)
         << "{"
         << "\"file_total\":" << storage.file_total << ','
         << "\"storage_bytes\":" << storage.storage_bytes << ','
         << "\"upload_count\":" << storage.upload_count << ','
         << "\"download_count\":" << storage.download_count << ','
         << "\"failed_request_count\":" << metrics.failed_requests << ','
         << "\"redis_hit_count\":" << metrics.metadata_cache_hits << ','
         << "\"redis_miss_count\":" << metrics.metadata_cache_misses << ','
         << "\"redis_error_count\":" << metrics.metadata_cache_errors << ','
         << "\"redis_hit_rate\":" << redis_hit_rate << ','
         << "\"status_codes\":" << statusCodesJson(metrics.status_codes)
         << "}\n";
    return HttpResponse::json(200, "OK", body.str());
}

HttpResponse HttpServer::handleAdminStatsStatusCodes(const HttpRequest&)
{
    const auto metrics = metrics_.snapshot();
    return HttpResponse::json(200, "OK", std::string("{\"status_codes\":")
                                  + statusCodesJson(metrics.status_codes) + "}\n");
}

HttpResponse HttpServer::handleAdminStatsRedis(const HttpRequest&)
{
    const auto metrics = metrics_.snapshot();
    const auto redis_total = metrics.metadata_cache_hits + metrics.metadata_cache_misses;
    const double redis_hit_rate = redis_total == 0
        ? 0.0
        : static_cast<double>(metrics.metadata_cache_hits) / static_cast<double>(redis_total);

    std::ostringstream body;
    body << std::fixed << std::setprecision(3)
         << "{"
         << "\"redis_hit_count\":" << metrics.metadata_cache_hits << ','
         << "\"redis_miss_count\":" << metrics.metadata_cache_misses << ','
         << "\"redis_error_count\":" << metrics.metadata_cache_errors << ','
         << "\"redis_hit_rate\":" << redis_hit_rate
         << "}\n";
    return HttpResponse::json(200, "OK", body.str());
}

void HttpServer::recordAudit(const HttpRequest& request, const HttpResponse& response,
                             const std::string& remote_addr,
                             std::chrono::steady_clock::time_point started_at)
{
    const auto action = actionForRequest(request);
    if (action.empty()) {
        return;
    }

    AuditLogRecord record;
    record.request_id = request.request_id.empty() ? "-" : request.request_id;
    record.user_id = request.user_id;
    record.username = request.username;
    record.method = httpMethodName(request.method);
    record.path = request.path;
    record.action = action;
    if (action == "download_object" || action == "delete_object") {
        record.file_id = extractObjectIdFromPath(request.path);
    }
    record.status_code = response.statusCode();
    record.result = response.statusCode() >= 200 && response.statusCode() < 400 ? "success" : "failure";
    record.client_ip = remote_addr;
    record.latency_ms = elapsedMs(started_at);
    record.created_at = nowIso();

    std::string error;
    if (!object_store_.metadataStore().insertAuditLog(record, error)) {
        logger_.error("cannot insert audit log: " + error);
    }
}

void HttpServer::logAccess(const std::string& remote, const std::string& method,
                           const std::string& path, int status_code,
                           std::size_t request_bytes, std::size_t response_bytes,
                           std::uint64_t duration_ms)
{
    metrics_.recordRequest(status_code, request_bytes, response_bytes, duration_ms);
    logger_.access(remote, method, path, status_code, response_bytes, duration_ms);
    if (duration_ms >= slow_request_ms_) {
        logger_.slow(remote, method, path, status_code, duration_ms);
    }
}

bool HttpServer::setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "fcntl F_GETFL failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "fcntl F_SETFL failed: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

bool HttpServer::sendAll(int fd, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace mini_oss
