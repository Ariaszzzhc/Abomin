#include "core/runtime.hpp"

#include <iostream>

#include "builtins.hpp"
#include "core/module_loader.hpp"
#include "core/value.hpp"

namespace Abomin {
  namespace {
    thread_local Runtime* active{};
    std::mutex outputMutex;
  }  // namespace

  struct WorkerState {
    std::shared_ptr<Inbox> inbox{std::make_shared<Inbox>()};
    std::shared_ptr<std::atomic_bool> stopped{std::make_shared<std::atomic_bool>(false)};
    std::thread thread;
    std::mutex engineMutex;
    Engine* engine{};

    void stop() {
      stopped->store(true);
      {
        std::lock_guard lock(engineMutex);
        if (engine) engine->interrupt();
      }
      inbox->send({Envelope::NetworkReady, 0, {}});
    }

    ~WorkerState() {
      stop();
      if (thread.joinable()) thread.join();
    }
  };

  bool Inbox::send(Envelope envelope) {
    std::lock_guard lock(mutex);
    if (closed) return false;
    if (envelope.kind == Envelope::NetworkReady) {
      if (wake) uv_async_send(wake);
      return true;
    }
    if (envelope.kind == Envelope::Message
        && (messages.size() >= 1024 || queuedBytes + envelope.payload.size() > 64 * 1024 * 1024))
      return false;
    queuedBytes += envelope.payload.size();
    messages.push_back(std::move(envelope));
    if (wake) uv_async_send(wake);
    return true;
  }

  void Inbox::post(std::function<void()> task) {
    std::lock_guard lock(mutex);
    if (closed) return;
    callbacks.push_back(std::move(task));
    if (wake) uv_async_send(wake);
  }

  FileResource::~FileResource() {
    if (fd >= 0) {
      uv_fs_t req{};
      uv_fs_close(nullptr, &req, fd, nullptr);
      uv_fs_req_cleanup(&req);
    }
  }

  Runtime& Runtime::current() {
    if (!active) throw std::runtime_error("No active runtime");
    return *active;
  }

  uv_loop_t* Runtime::currentLoop() {
    return active ? active->loop() : nullptr;
  }

  Runtime::Runtime(std::vector<std::string> arguments, std::shared_ptr<Inbox> inbox,
                   std::shared_ptr<Inbox> parent, std::uint32_t workerId,
                   std::shared_ptr<std::atomic_bool> stopped)
      : args(std::move(arguments)),
        inbox_(inbox ? std::move(inbox) : std::make_shared<Inbox>()),
        parent_(std::move(parent)),
        workerId_(workerId),
        stopped_(stopped ? std::move(stopped) : std::make_shared<std::atomic_bool>(false)) {
    active = this;
    if (uv_loop_init(&loop_) < 0) throw std::runtime_error("Cannot initialize event loop");
    uv_async_init(&loop_, &wake_, [](uv_async_t*) {});
    {
      std::lock_guard lock(inbox_->mutex);
      inbox_->wake = &wake_;
    }
    if (!isWorker()) uv_unref(reinterpret_cast<uv_handle_t*>(&wake_));
    engine_ = std::make_unique<Engine>(*stopped_);
    installOps(*this);
  }

  Runtime::~Runtime() {
    stopped_->store(true);
    workers_.clear();
    shutdownNetwork(*this);
    closePorts(inbox_);
    {
      std::lock_guard lock(inbox_->mutex);
      inbox_->closed = true;
      inbox_->wake = nullptr;
    }
    files_.clear();
    while (!timers_.empty()) cancelTimer(timers_.begin()->first);
    if (dispatcher_) JSValueUnprotect(context(), dispatcher_);
    engine_.reset();
    uv_close(reinterpret_cast<uv_handle_t*>(&wake_), nullptr);
    // Drain cancelled I/O before releasing native allocations. 排空已取消 I/O 后再释放原生内存。
    uv_run(&loop_, UV_RUN_DEFAULT);
    if (uv_loop_close(&loop_) != 0) std::terminate();
    active = nullptr;
  }

  void Runtime::setDispatcher(JSObjectRef callback) {
    if (dispatcher_) JSValueUnprotect(context(), dispatcher_);
    dispatcher_ = callback;
    JSValueProtect(context(), dispatcher_);
  }

  void Runtime::dispatch(std::string_view type, std::uint32_t id, JSValueRef value) {
    if (!dispatcher_) return;
    JSValueRef values[] = {string(context(), type), JSValueMakeNumber(context(), id), value};
    call(context(), dispatcher_, values);
  }

  void Runtime::complete(std::uint32_t id, bool ok, std::function<JSValueRef()> value) {
    enqueue([this, id, ok, value = std::move(value)] {
      dispatch(ok ? "resolve" : "reject", id, value());
    });
  }

  void Runtime::report(std::string message) {
    exitCode = 1;
    requestStop();
    if (parent_)
      parent_->send({Envelope::WorkerError, workerId_, message});
    else {
      std::lock_guard lock(outputMutex);
      std::cerr << message << '\n';
    }
  }

  int Runtime::run(const std::string& input, bool eval) {
    try {
      entry = eval ? "abomin:eval" : ModuleLoader::resolve(input);
      engine_->evaluate(bootstrap, "abomin:bootstrap");
      auto done = engine_->evaluate("globalThis.__abominStart", "abomin:start");
      JSValueRef result;
      if (eval) {
        auto source = "(async()=>{\n" + input + "\n})()";
        result = engine_->evaluate(source, "abomin:eval");
      } else
        result = engine_->import(entry);
      JSValueRef values[] = {result};
      call(context(), JSValueToObject(context(), done, nullptr), values);
      engine_->checkpoint();
      while (!stopped()) {
        pollNetwork(*this);
        drainInbox();
        while (!tasks_.empty() && !stopped()) {
          auto task = std::move(tasks_.front());
          tasks_.pop_front();
          task();
          engine_->checkpoint();
        }
        engine_->checkpoint();
        if (stopped()) break;
        if (!tasks_.empty()) continue;
        bool hasMessages, hasCallbacks;
        {
          std::lock_guard lock(inbox_->mutex);
          hasMessages = !inbox_->messages.empty();
          hasCallbacks = !inbox_->callbacks.empty();
        }
        if (hasCallbacks
            || (hasMessages
                && (!isWorker()
                    || JSValueToBoolean(context(), engine_->evaluate("globalThis.__abominSettled",
                                                                     "abomin:state")))))
          continue;
        if (!uv_loop_alive(&loop_)) {
          auto settled = engine_->evaluate("globalThis.__abominSettled", "abomin:state");
          if (!JSValueToBoolean(context(), settled))
            report("Module evaluation ended with an unresolved top-level await");
          break;
        }
        uv_run(&loop_, UV_RUN_ONCE);
      }
    } catch (const std::exception& e) {
      if (!stopped()) report(e.what());
    }
    return exitCode;
  }

  std::uint32_t Runtime::timer(double delay, bool repeat) {
    auto id = nextResource_++;
    auto* timer = new uv_timer_t{};
    uv_timer_init(&loop_, timer);
    timer->data = new std::pair<Runtime*, std::uint32_t>(this, id);
    timers_[id] = timer;
    auto ms = static_cast<std::uint64_t>(std::clamp(delay, 1.0, 2147483647.0));
    uv_timer_start(
        timer,
        [](uv_timer_t* timer) {
          auto [runtime, id] = *static_cast<std::pair<Runtime*, std::uint32_t>*>(timer->data);
          runtime->enqueue([runtime, id] {
            runtime->dispatch("timer", id, JSValueMakeUndefined(runtime->context()));
          });
          if (!uv_timer_get_repeat(timer)) runtime->cancelTimer(id);
        },
        ms, repeat ? ms : 0);
    return id;
  }

  void Runtime::cancelTimer(std::uint32_t id) {
    auto found = timers_.find(id);
    if (found == timers_.end()) return;
    auto* timer = found->second;
    timers_.erase(found);
    uv_timer_stop(timer);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), [](uv_handle_t* h) {
      auto* timer = reinterpret_cast<uv_timer_t*>(h);
      delete static_cast<std::pair<Runtime*, std::uint32_t>*>(timer->data);
      delete timer;
    });
  }

  std::uint32_t Runtime::addFile(uv_file fd) {
    auto id = nextResource_++;
    auto resource = std::make_shared<FileResource>();
    resource->fd = fd;
    files_[id] = std::move(resource);
    return id;
  }

  std::shared_ptr<FileResource> Runtime::file(std::uint32_t id) {
    auto found = files_.find(id);
    if (found == files_.end()) throw std::runtime_error("Invalid file resource");
    return found->second;
  }

  void Runtime::closeFile(std::uint32_t id) {
    if (!files_.erase(id)) throw std::runtime_error("Invalid file resource");
  }

  std::uint32_t Runtime::worker(const std::string& url) {
    auto id = nextResource_++;
    auto state = std::make_unique<WorkerState>();
    auto inbox = state->inbox;
    auto stopped = state->stopped;
    auto parent = inbox_;
    auto* owner = state.get();
    state->thread = std::thread([id, url, inbox, stopped, parent, owner] {
      try {
        Runtime runtime({}, inbox, parent, id, stopped);
        {
          std::lock_guard lock(owner->engineMutex);
          owner->engine = runtime.engine();
          if (stopped->load()) owner->engine->interrupt();
        }
        runtime.run(url);
        {
          std::lock_guard lock(owner->engineMutex);
          owner->engine = nullptr;
        }
      } catch (const std::exception& e) {
        parent->send({Envelope::WorkerError, id, e.what()});
      }
      parent->send({Envelope::WorkerExit, id, {}});
    });
    workers_[id] = std::move(state);
    retain();
    return id;
  }

  void Runtime::postWorker(std::uint32_t id, std::string packet) {
    auto found = workers_.find(id);
    if (found == workers_.end()) return;
    if (!found->second->inbox->send({Envelope::Message, 0, std::move(packet)}))
      throw std::runtime_error("Worker mailbox is full or closed");
  }

  void Runtime::terminateWorker(std::uint32_t id) {
    auto found = workers_.find(id);
    if (found == workers_.end()) return;
    found->second->stop();
  }

  void Runtime::postParent(std::string packet) {
    if (!parent_) throw std::runtime_error("postMessage is only available in a Worker");
    if (!parent_->send({Envelope::Message, workerId_, std::move(packet)}))
      throw std::runtime_error("Parent mailbox is full or closed");
  }

  void Runtime::drainInbox() {
    std::deque<Envelope> incoming;
    std::deque<std::function<void()>> callbacks;
    {
      std::lock_guard lock(inbox_->mutex);
      incoming.swap(inbox_->messages);
      callbacks.swap(inbox_->callbacks);
      inbox_->queuedBytes = 0;
    }
    for (auto& callback : callbacks) {
      callback();
      engine_->checkpoint();
    }
    for (auto& envelope : incoming) {
      if (envelope.kind == Envelope::NetworkReady) continue;
      if (envelope.kind == Envelope::WorkerExit) {
        workers_.erase(envelope.id);
        release();
        dispatch("workerExit", envelope.id, JSValueMakeUndefined(context()));
      } else if (envelope.kind == Envelope::PortReady)
        deliverPort(*this, envelope.id);
      else {
        if (isWorker() && envelope.kind == Envelope::Message
            && !JSValueToBoolean(context(),
                                 engine_->evaluate("globalThis.__abominSettled", "abomin:state"))) {
          std::lock_guard lock(inbox_->mutex);
          inbox_->queuedBytes += envelope.payload.size();
          inbox_->messages.push_back(std::move(envelope));
          continue;
        }
        dispatch(envelope.kind == Envelope::WorkerError ? "workerError" : "message", envelope.id,
                 string(context(), envelope.payload));
      }
      engine_->checkpoint();
    }
  }
}  // namespace Abomin
