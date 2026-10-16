#include "core/compute_pool.hpp"

#include <algorithm>

namespace Abomin {
  ComputePool& ComputePool::shared() {
    static ComputePool pool;
    return pool;
  }

  ComputePool::ComputePool() {
    auto count = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    for (unsigned i = 0; i < count; ++i)
      threads_.emplace_back([this] {
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
          }
          task();
        }
      });
  }

  ComputePool::~ComputePool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    for (auto& thread : threads_) thread.join();
  }

  bool ComputePool::submit(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || queue_.size() >= 256) return false;
      queue_.push_back(std::move(task));
    }
    condition_.notify_one();
    return true;
  }
}  // namespace Abomin
