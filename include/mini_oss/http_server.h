#pragma once

#include "mini_oss/object_store.h"
#include "mini_oss/router.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace mini_oss {

class HttpServer {
public:
    explicit HttpServer(std::uint16_t port);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void run(const std::function<bool()>& keep_running);
    void stop();

private:
    void acceptClients();
    void handleClientRead(int client_fd);
    void closeClient(int client_fd);

    static bool setNonBlocking(int fd);
    static bool sendAll(int fd, const std::string& data);

    std::uint16_t port_ = 0;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    std::unordered_map<int, std::string> buffers_;
    ObjectStore object_store_;
    Router router_;
};

} // namespace mini_oss
