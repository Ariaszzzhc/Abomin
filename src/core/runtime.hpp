#pragma once
#include <uv.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/engine.hpp"

namespace Abomin {
  struct Envelope {
    enum Kind { Message, WorkerError, WorkerExit, PortReady, NetworkReady } kind;

    std::uint32_t id{};
    std::string payload;
  };

  /** Mailboxes contain owned data only; no JS value crosses a thread boundary.
   * 邮箱仅含独立数据；JS 值绝不跨线程传递。 */
  struct Inbox {
    std::mutex mutex;
    std::deque<Envelope> messages;
    uv_async_t* wake{};
    bool closed{};
    std::deque<std::function<void()>> callbacks;
    std::size_t queuedBytes{};
    bool send(Envelope envelope);
    void post(std::function<void()> task);
  };

  struct FileResource {
    uv_file fd{-1};
    ~FileResource();
  };
  struct WorkerState;

  class Runtime {
  public:
    Runtime(std::vector<std::string> args, std::shared_ptr<Inbox> inbox = {},
            std::shared_ptr<Inbox> parent = {}, std::uint32_t workerId = 0,
            std::shared_ptr<std::atomic_bool> stopped = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    int run(const std::string& entry, bool eval = false);

    JSContextRef context() const {
      return engine_->context();
    }

    Engine* engine() {
      return engine_.get();
    }

    uv_loop_t* loop() {
      return &loop_;
    }

    void complete(std::uint32_t id, bool ok, std::function<JSValueRef()> value);

    void enqueue(std::function<void()> task) {
      tasks_.push_back(std::move(task));
    }

    void dispatch(std::string_view type, std::uint32_t id, JSValueRef value);
    void setDispatcher(JSObjectRef callback);
    void report(std::string message);

    void requestStop() {
      stopped_->store(true);
    }

    bool stopped() const {
      return stopped_->load();
    }

    std::uint32_t timer(double delay, bool repeat);
    void cancelTimer(std::uint32_t id);
    std::uint32_t worker(const std::string& url);
    void postWorker(std::uint32_t id, std::string packet);
    void terminateWorker(std::uint32_t id);
    void postParent(std::string packet);
    std::uint32_t addFile(uv_file fd);
    std::shared_ptr<FileResource> file(std::uint32_t id);
    void closeFile(std::uint32_t id);

    std::shared_ptr<Inbox> inbox() {
      return inbox_;
    }

    static Runtime& current();
    static uv_loop_t* currentLoop();
    std::vector<std::string> args;
    std::string entry;

    bool isWorker() const {
      return bool(parent_);
    }

    void retain() {
      ++retained_;
      uv_ref(reinterpret_cast<uv_handle_t*>(&wake_));
    }

    void release() {
      if (--retained_ == 0 && !isWorker()) uv_unref(reinterpret_cast<uv_handle_t*>(&wake_));
    }

    int exitCode{};

  private:
    void drainInbox();
    uv_loop_t loop_{};
    uv_async_t wake_{};
    std::shared_ptr<Inbox> inbox_, parent_;
    std::uint32_t workerId_{};
    std::shared_ptr<std::atomic_bool> stopped_;
    std::unique_ptr<Engine> engine_;
    JSObjectRef dispatcher_{};
    std::deque<std::function<void()>> tasks_;
    std::unordered_map<std::uint32_t, uv_timer_t*> timers_;
    std::unordered_map<std::uint32_t, std::shared_ptr<FileResource>> files_;
    std::unordered_map<std::uint32_t, std::unique_ptr<WorkerState>> workers_;
    std::uint32_t nextResource_{1};
    unsigned retained_{};
  };

  void pollNetwork(Runtime&);
  void shutdownNetwork(Runtime&);
  void installOps(Runtime& runtime);
  void closePorts(const std::shared_ptr<Inbox>& inbox);
  void deliverPort(Runtime& runtime, std::uint32_t id);
}  // namespace Abomin
