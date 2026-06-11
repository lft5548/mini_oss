#include "mini_oss/http_server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <queue>
#include <sstream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
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

} // namespace

HttpServer::HttpServer(std::uint16_t port, std::size_t worker_threads,
                       std::filesystem::path storage_dir, Logger& logger,
                       std::uint64_t slow_request_ms, std::string auth_token,
                       std::size_t max_request_bytes, std::size_t max_upload_bytes,
                       std::size_t stream_upload_threshold_bytes,
                       std::size_t max_connections, std::size_t thread_queue_limit,
                       std::uint64_t request_timeout_ms, std::uint64_t upload_timeout_ms)
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
    , object_store_(std::move(storage_dir))
    , thread_pool_(worker_threads, thread_queue_limit)
{
    if (stream_upload_threshold_bytes_ > max_upload_bytes_) {
        stream_upload_threshold_bytes_ = max_upload_bytes_;
    }

    router_.addRoute(HttpMethod::Get, "/health", [](const HttpRequest&) {
        return HttpResponse::json(200, "OK", "{\"status\":\"ok\"}\n");
    });
    router_.addRoute(HttpMethod::Get, "/metrics", [this](const HttpRequest&) {
        return HttpResponse::json(200, "OK", metrics_.toJson() + "\n");
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
                    if (!isAuthorized(head_result.request)) {
                        sendImmediateResponse(client_fd, HttpResponse::unauthorized(),
                                              httpMethodName(head_result.request.method),
                                              head_result.request.path, body_begin);
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

                    if (!beginStreamingUpload(client_fd, state, head_result.request,
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

        if (!isAuthorized(request)) {
            http_response = HttpResponse::unauthorized();
        } else {
            http_response = router_.route(request);
            if (object_upload && http_response.statusCode() >= 200
                && http_response.statusCode() < 300) {
                metrics_.recordObjectUpload(upload_body_size, streamed_upload);
            }
        }
        cleanupTemporaryRequestBody(request);

        const auto serialized = http_response.serialize();
        const auto duration_ms = elapsedMs(started_at);
        logAccess(remote_addr, method, path, http_response.statusCode(), request_bytes,
                  serialized.size(), duration_ms);
        enqueueResponse(client_fd, generation, std::move(serialized));
    });

    if (!queued) {
        metrics_.queueRejected();
        cleanupTemporaryRequestBody(request);
        const auto response = HttpResponse::text(503, "Service Unavailable", "server busy\n");
        const auto serialized = response.serialize();
        logAccess(remote_addr, httpMethodName(request.method), request.path, response.statusCode(),
                  request_bytes, serialized.size(), elapsedMs(started_at));
        enqueueResponse(client_fd, generation, serialized);
    }
}

void HttpServer::enqueueResponse(int client_fd, std::uint64_t generation, std::string response)
{
    {
        std::lock_guard<std::mutex> lock(responses_mutex_);
        responses_.push(PendingResponse {client_fd, generation, std::move(response)});
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

        sendAll(response.client_fd, response.response);
        closeClient(response.client_fd);
    }
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
    return request.path == "/objects" || request.path.rfind("/objects/", 0) == 0;
}

bool HttpServer::isAuthorized(const HttpRequest& request) const
{
    if (auth_token_.empty() || !requiresAuth(request)) {
        return true;
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
