#include "mini_oss/http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace mini_oss {
namespace {

constexpr int kBacklog = 128;
constexpr int kMaxEvents = 1024;
constexpr std::uint64_t kWakeupValue = 1;
constexpr int kBufferSize = 4096;
constexpr std::size_t kMaxHeaderSize = 16 * 1024;
constexpr std::size_t kMaxRequestSize = 10 * 1024 * 1024;

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

} // namespace

HttpServer::HttpServer(std::uint16_t port, std::size_t worker_threads,
                       std::filesystem::path storage_dir, Logger& logger,
                       std::uint64_t slow_request_ms)
    : port_(port)
    , logger_(logger)
    , slow_request_ms_(slow_request_ms)
    , object_store_(std::move(storage_dir))
    , thread_pool_(worker_threads)
{
    router_.addRoute(HttpMethod::Get, "/health", [](const HttpRequest&) {
        return HttpResponse::json(200, "OK", "{\"status\":\"ok\"}\n");
    });
    router_.addRoute(HttpMethod::Get, "/metrics", [this](const HttpRequest&) {
        return HttpResponse::json(200, "OK", metrics_.toJson() + "\n");
    });
    router_.addRoute(HttpMethod::Post, "/objects", [this](const HttpRequest& request) {
        return object_store_.createObject(request);
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
        const int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, 1000);
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
    }

    sendCompletedResponses();
}

void HttpServer::stop()
{
    for (const auto& item : clients_) {
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

        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            logServerError(socketError("epoll_ctl client fd"));
            ::close(client_fd);
            continue;
        }

        char addr_text[INET_ADDRSTRLEN] {};
        const char* remote = ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_text, sizeof(addr_text));
        std::ostringstream remote_addr;
        remote_addr << (remote == nullptr ? "unknown" : remote) << ':' << ntohs(client_addr.sin_port);

        clients_[client_fd] =
            ClientState {{}, remote_addr.str(), std::chrono::steady_clock::now(), next_generation_++, false};
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
    auto& request = client_it->second.buffer;

    while (true) {
        const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            request.append(buffer, static_cast<std::size_t>(n));
            const auto header_end = request.find("\r\n\r\n");
            if (header_end == std::string::npos && request.size() > kMaxHeaderSize) {
                const auto response = HttpResponse::text(431, "Request Header Fields Too Large",
                                                         "request header too large\n");
                const auto serialized = response.serialize();
                const auto duration_ms = elapsedMs(client_it->second.started_at);
                sendAll(client_fd, serialized);
                logAccess(client_it->second.remote_addr, "-", "-", response.statusCode(), request.size(),
                          serialized.size(), duration_ms);
                closeClient(client_fd);
                return;
            }
            if (request.size() > kMaxRequestSize) {
                const auto response = HttpResponse::text(413, "Payload Too Large",
                                                         "request body too large\n");
                const auto serialized = response.serialize();
                const auto duration_ms = elapsedMs(client_it->second.started_at);
                sendAll(client_fd, serialized);
                logAccess(client_it->second.remote_addr, "-", "-", response.statusCode(), request.size(),
                          serialized.size(), duration_ms);
                closeClient(client_fd);
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

    const auto parse_result = parseHttpRequest(request);
    if (!parse_result.complete) {
        return;
    }

    const std::uint64_t generation = client_it->second.generation;
    const std::string remote_addr = client_it->second.remote_addr;
    const auto started_at = client_it->second.started_at;
    client_it->second.processing = true;

    epoll_event event {};
    event.events = EPOLLRDHUP;
    event.data.fd = client_fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event) < 0) {
        logServerError(socketError("epoll_ctl disable read"));
        closeClient(client_fd);
        return;
    }

    submitRequest(client_fd, generation, remote_addr, started_at, std::move(request));
}

void HttpServer::submitRequest(int client_fd, std::uint64_t generation, std::string remote_addr,
                               std::chrono::steady_clock::time_point started_at,
                               std::string raw_request)
{
    const bool queued = thread_pool_.enqueue([this, client_fd, generation, remote_addr = std::move(remote_addr),
                                              started_at, raw_request = std::move(raw_request)] {
        const auto parse_result = parseHttpRequest(raw_request);
        HttpResponse http_response = HttpResponse::badRequest("invalid request");
        std::string method = "-";
        std::string path = "-";
        if (!parse_result.complete || !parse_result.ok) {
            const std::string error = parse_result.error.empty() ? "invalid request" : parse_result.error;
            http_response = HttpResponse::badRequest(error);
        } else {
            method = httpMethodName(parse_result.request.method);
            path = parse_result.request.path;
            http_response = router_.route(parse_result.request);
        }

        const auto serialized = http_response.serialize();
        const auto duration_ms = elapsedMs(started_at);
        logAccess(remote_addr, method, path, http_response.statusCode(), raw_request.size(),
                  serialized.size(), duration_ms);
        enqueueResponse(client_fd, generation, std::move(serialized));
    });

    if (!queued) {
        const auto response = HttpResponse::text(503, "Service Unavailable", "server shutting down\n");
        const auto serialized = response.serialize();
        logAccess(remote_addr, "-", "-", response.statusCode(), 0, serialized.size(),
                  elapsedMs(started_at));
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
    if (clients_.erase(client_fd) > 0) {
        metrics_.connectionClosed();
    }
    ::close(client_fd);
}

void HttpServer::logServerError(const std::string& message)
{
    std::cerr << message << '\n';
    logger_.error(message);
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
