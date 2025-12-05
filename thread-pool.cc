#include "thread-pool.h"
#include "http-server.h"
#include <iostream>
#include <memory> // for std::packaged_task
#include <stdexcept>
#include <type_traits>

ThreadPool::ThreadPool(size_t threads) {
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0)
            threads = 4;
    }

    std::cout << "Starting thread pool with " << threads << " worker threads." << std::endl;

    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] { this->worker_loop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::shutdown() {
    bool expected = false;
    if (!stop_flag.compare_exchange_strong(expected, true)) {
        return;
    }

    condition.notify_all();

    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
    std::cout << "Thread pool shut down successfully." << std::endl;
}

void ThreadPool::worker_loop() {
    while (!stop_flag) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);

            condition.wait(lock, [this] { return stop_flag || !tasks.empty(); });

            if (stop_flag && tasks.empty()) {
                return;
            }

            task = std::move(tasks.front());
            tasks.pop();
        }

        // Execute the task
        task();
    }
}

template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args) -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task =
        std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        if (stop_flag)
            throw std::runtime_error("enqueue on stopped ThreadPool");

        tasks.emplace([task]() { (*task)(); });
    }
    condition.notify_one();
    return res;
}

// Explicitly instantiate enqueue for the common void return type (used by handle_client)
template std::future<void>
ThreadPool::enqueue<void (HttpServer::*)(int), HttpServer *, int &>(void (HttpServer::*&&)(int), HttpServer *&&, int &);
