#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace raybotic::scan::detail {

/// Fixed-size pool for chunk compression.
class ThreadPool {
  public:
    explicit ThreadPool(unsigned threads) {
        if (threads == 0) threads = 1;
        for (unsigned i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { run(); });
        }
    }

    ~ThreadPool() {
        {
            std::scoped_lock lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F>
    auto submit(F&& f) -> std::future<decltype(f())> {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto future = task->get_future();
        {
            std::scoped_lock lock(mutex_);
            jobs_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return future;
    }

  private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stop_{false};
};

}  // namespace raybotic::scan::detail
