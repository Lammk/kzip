#pragma once
// Minimal ThreadPool using std::thread (equivalent to std::jthread).
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace kzip {

class ThreadPool {
 public:
  explicit ThreadPool(size_t n = 0) : stop_(false) {
    if (n == 0) n = 1;
    for (size_t i = 0; i < n; ++i)
      workers_.emplace_back([this] {
        for (;;) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
          }
          task();
        }
      });
  }
  ~ThreadPool() {
    { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  template <class F>
  auto enqueue(F&& f) -> std::future<decltype(f())> {
    using R = decltype(f());
    auto p = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
    std::future<R> fu = p->get_future();
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks_.emplace([p] { (*p)(); });
    }
    cv_.notify_one();
    return fu;
  }
  size_t size() const { return workers_.size(); }
 private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_;
};

} // namespace kzip
