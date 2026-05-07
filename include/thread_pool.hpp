#pragma once

// Single-queue thread pool built on std::jthread + stop_token.
// Designed for query-level parallelism in the search engine where individual
// tasks are short, uniform, and lock-free against each other.

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <cstddef>

namespace idx::concurrent {

class ThreadPool {
public:
    explicit ThreadPool(unsigned n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 1;
        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            workers_.emplace_back([this](std::stop_token st) { run(st); });
        }
    }

    ~ThreadPool() {
        // Member destruction follows reverse declaration order, so cv_ and
        // m_ would otherwise be torn down BEFORE the jthreads have joined,
        // leaving any worker still inside cv_.wait() reading a destroyed
        // condition variable. Force join now while cv_ / m_ are alive.
        for (auto& w : workers_) w.request_stop();
        cv_.notify_all();
        workers_.clear();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        {
            std::lock_guard lk(m_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    std::size_t size() const noexcept { return workers_.size(); }

private:
    // The stop_token-aware overload of std::condition_variable_any::wait
    // ships with libstdc++ but is unreliable on AppleClang 17 / libc++
    // (manifests as "mutex lock failed: Invalid argument"). We therefore
    // bake the stop check straight into the predicate and use the plain
    // wait overload, which is correct on both stdlibs.
    void run(std::stop_token st) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return st.stop_requested() || !tasks_.empty(); });
                if (st.stop_requested() && tasks_.empty()) return;
                job = std::move(tasks_.front());
                tasks_.pop();
            }
            job();
        }
    }

    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
};

}  // namespace idx::concurrent
