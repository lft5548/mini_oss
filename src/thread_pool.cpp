#include "mini_oss/thread_pool.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <utility>

namespace mini_oss {

ThreadPool::ThreadPool(std::size_t thread_count, std::size_t max_queue_size)
    : max_queue_size_(max_queue_size)
{
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) {
            thread_count = 4;
        }
    }
    thread_count = std::max<std::size_t>(1, thread_count);

    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this] {
            workerLoop();
        });
    }
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

bool ThreadPool::enqueue(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_)) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) {
            return;
        }
        stopping_ = true;
    }

    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::size_t ThreadPool::threadCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

void ThreadPool::workerLoop()
{
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception& ex) {
            std::cerr << "thread pool task failed: " << ex.what() << '\n';
        } catch (...) {
            std::cerr << "thread pool task failed: unknown exception\n";
        }
    }
}

} // namespace mini_oss
