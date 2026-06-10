#pragma once

#include "mini_oss/object_store.h"
#include "mini_oss/router.h"
#include "mini_oss/thread_pool.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace mini_oss {

class HttpServer {
public:
    explicit HttpServer(std::uint16_t port, std::size_t worker_threads = 0);
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
    void submitRequest(int client_fd, std::uint64_t generation, std::string raw_request);
    void enqueueResponse(int client_fd, std::uint64_t generation, std::string response);
    void sendCompletedResponses();
    void closeClient(int client_fd);

    static bool setNonBlocking(int fd);
    static bool sendAll(int fd, const std::string& data);

    struct ClientState {
        std::string buffer;
        std::uint64_t generation = 0;
        bool processing = false;
    };

    struct PendingResponse {
        int client_fd = -1;
        std::uint64_t generation = 0;
        std::string response;
    };

    std::uint16_t port_ = 0;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    std::uint64_t next_generation_ = 1;
    std::unordered_map<int, ClientState> clients_;
    std::mutex responses_mutex_;
    std::queue<PendingResponse> responses_;
    ObjectStore object_store_;
    Router router_;
    ThreadPool thread_pool_;
};

} // namespace mini_oss
