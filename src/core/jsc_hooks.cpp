#include <algorithm>
#include <cstdlib>
#include <set>

#include "core/runtime.hpp"

/** Adapt engine timers without letting an engine cache retain a dead uv loop.
 * 适配引擎定时器，避免引擎缓存持有已销毁的 uv 循环。 */
extern "C" {
void WTFTimer__fire(void*);

struct EngineTimer {
  uv_timer_t timer{};
  void* owner{};
  uv_loop_t* loop{};
  bool ownerAlive{true};
  bool closing{}, handleClosed{};
};
}

namespace {
  // Engine thread-local caches can be destroyed after C++ static objects.
  // 引擎线程缓存可能晚于 C++ 静态对象销毁，因此注册表采用进程生命周期。
  struct TimerRegistry {
    std::mutex mutex;
    std::set<EngineTimer*> timers;
  };

  TimerRegistry& registry() {
    static auto* value = new TimerRegistry;
    return *value;
  }

  void closeHandle(EngineTimer* timer) {
    if (timer->closing) return;
    timer->closing = true;
    timer->loop = nullptr;
    uv_timer_stop(&timer->timer);
    uv_close(reinterpret_cast<uv_handle_t*>(&timer->timer), [](uv_handle_t* handle) {
      auto* timer = static_cast<EngineTimer*>(handle->data);
      std::lock_guard lock(registry().mutex);
      timer->handleClosed = true;
      if (!timer->ownerAlive) {
        registry().timers.erase(timer);
        delete timer;
      }
    });
  }
}  // namespace

namespace Abomin {
  void shutdownEngineTimers(uv_loop_t* loop) {
    std::lock_guard lock(registry().mutex);
    for (auto* timer : registry().timers)
      if (timer->loop == loop) closeHandle(timer);
  }
}  // namespace Abomin

extern "C" {
/** VM threads use libuv; helper threads retain their generic loop.
 * VM 所属线程使用 libuv；辅助线程保留通用循环。 */
bool Bun__thisThreadHasVM() {
  return Abomin::Runtime::currentLoop() != nullptr;
}

EngineTimer* WTFTimer__create(void* owner) {
  auto* loop = Abomin::Runtime::currentLoop();
  if (!loop) return nullptr;
  auto* timer = new EngineTimer{};
  timer->owner = owner;
  timer->loop = loop;
  uv_timer_init(loop, &timer->timer);
  timer->timer.data = timer;
  uv_unref(reinterpret_cast<uv_handle_t*>(&timer->timer));
  {
    std::lock_guard lock(registry().mutex);
    registry().timers.insert(timer);
  }
  return timer;
}

void WTFTimer__update(EngineTimer* timer, double seconds, bool repeat) {
  std::lock_guard lock(registry().mutex);
  if (timer->closing) return;
  auto ms = static_cast<std::uint64_t>(std::max(1.0, seconds * 1000.0));
  uv_timer_start(
      &timer->timer,
      [](uv_timer_t* handle) {
        auto* timer = static_cast<EngineTimer*>(handle->data);
        WTFTimer__fire(timer->owner);
      },
      ms, repeat ? ms : 0);
}

void WTFTimer__deinit(EngineTimer* timer) {
  std::lock_guard lock(registry().mutex);
  timer->ownerAlive = false;
  if (timer->handleClosed) {
    registry().timers.erase(timer);
    delete timer;
  } else
    closeHandle(timer);
}

bool WTFTimer__isActive(const EngineTimer* timer) {
  std::lock_guard lock(registry().mutex);
  return !timer->closing && uv_is_active(reinterpret_cast<const uv_handle_t*>(&timer->timer));
}

double WTFTimer__secondsUntilTimer(const EngineTimer* timer) {
  std::lock_guard lock(registry().mutex);
  return timer->closing ? -1.0 : static_cast<double>(uv_timer_get_due_in(&timer->timer)) / 1000.0;
}

void WTFTimer__cancel(EngineTimer* timer) {
  std::lock_guard lock(registry().mutex);
  if (!timer->closing) uv_timer_stop(&timer->timer);
}

/** Host Error instances never carry Bun-specific native payloads.
 * 宿主 Error 实例不包含 Bun 专用原生负载。 */
void Bun__errorInstance__finalize(void*) {
  std::abort();
}

void Bun__reportUnhandledError(void*, std::int64_t) {
  Abomin::Runtime::current().report("Uncaught exception in microtask");
}
}
