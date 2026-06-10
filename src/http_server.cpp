#include "mini_oss/http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mini_oss {
namespace {

constexpr int kBacklog = 128;
constexpr int kMaxEvents = 1024;
constexpr int kBufferSize = 4096;
constexpr int kMaxHeaderSize = 16 * 1024;

} // namespace

HttpServer::HttpServer(std::uint16_t port)
    : port_(port)
{
    router_.addRoute(HttpMethod::Get, "/health", [](const HttpRequest&) {
        return HttpResponse::json(200, "OK", "{\"status\":\"ok\"}\n");
    });
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start()
{
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return false;
    }

    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed: " << std::strerror(errno) << '\n';
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
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if (::listen(listen_fd_, kBacklog) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        return false;
    }

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << '\n';
        return false;
    }

    epoll_event event {};
    event.events = EPOLLIN;
    event.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        std::cerr << "epoll_ctl listen fd failed: " << std::strerror(errno) << '\n';
        return false;
    }

    std::cout << "Mini-OSS HTTP server listening on 0.0.0.0:" << port_ << '\n';
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
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listen_fd_) {
                acceptClients();
            } else if ((events[i].events & EPOLLIN) != 0) {
                handleClientRead(fd);
            } else {
                closeClient(fd);
            }
        }
    }
}

void HttpServer::stop()
{
    for (const auto& item : buffers_) {
        ::close(item.first);
    }
    buffers_.clear();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
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
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            break;
        }

        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            std::cerr << "epoll_ctl client fd failed: " << std::strerror(errno) << '\n';
            ::close(client_fd);
            continue;
        }

        buffers_[client_fd] = {};
    }
}

void HttpServer::handleClientRead(int client_fd)
{
    char buffer[kBufferSize];
    auto& request = buffers_[client_fd];

    while (true) {
        const ssize_t n = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            request.append(buffer, static_cast<std::size_t>(n));
            if (request.size() > kMaxHeaderSize) {
                const auto response = HttpResponse::text(413, "Payload Too Large",
                                                         "request header too large\n")
                                          .serialize();
                sendAll(client_fd, response);
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

        std::cerr << "recv failed: " << std::strerror(errno) << '\n';
        closeClient(client_fd);
        return;
    }

    const auto parse_result = parseHttpRequest(request);
    if (!parse_result.complete) {
        return;
    }

    std::string response;
    if (!parse_result.ok) {
        response = HttpResponse::badRequest(parse_result.error).serialize();
    } else {
        response = router_.route(parse_result.request).serialize();
    }

    sendAll(client_fd, response);
    closeClient(client_fd);
}

void HttpServer::closeClient(int client_fd)
{
    if (epoll_fd_ >= 0) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    }
    buffers_.erase(client_fd);
    ::close(client_fd);
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
