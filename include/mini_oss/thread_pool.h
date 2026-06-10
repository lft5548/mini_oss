#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace mini_oss {

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(std::size_t thread_count = 0, std::size_t max_queue_size = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool enqueue(Task task);
    void shutdown();
    std::size_t threadCount() const;

private:
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    std::vector<std::thread> workers_;
    std::size_t max_queue_size_ = 0;
    bool stopping_ = false;
};

} // namespace mini_oss
