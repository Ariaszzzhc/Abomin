#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Abomin {
  /** Process-wide bounded native computation; work items contain no JS values.
   * 进程共享的有界原生计算池；任务不包含 JS 值。 */
  class ComputePool {
  public:
    static ComputePool& shared();
    bool submit(std::function<void()> task);
    ~ComputePool();

  private:
    ComputePool();
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool stopping_{};
  };
}  // namespace Abomin
