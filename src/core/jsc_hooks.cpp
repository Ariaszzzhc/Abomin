#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "core/runtime.hpp"

/** Adapt the prebuilt engine's timer hooks to the owning libuv loop.
 * 将预编译引擎的定时器钩子适配至所属 libuv 循环。 */
extern "C" {
void WTFTimer__fire(void*);

struct EngineTimer {
  uv_timer_t timer{};
  void* owner{};
};

EngineTimer* WTFTimer__create(void* owner) {
  auto* loop = Abomin::Runtime::currentLoop();
  if (!loop) return nullptr;
  auto* t = new EngineTimer{};
  t->owner = owner;
  uv_timer_init(loop, &t->timer);
  t->timer.data = t;
  uv_unref(reinterpret_cast<uv_handle_t*>(&t->timer));
  return t;
}

void WTFTimer__update(EngineTimer* t, double seconds, bool repeat) {
  auto ms = static_cast<std::uint64_t>(std::max(1.0, seconds * 1000.0));
  uv_timer_start(
      &t->timer,
      [](uv_timer_t* h) {
        auto* t = static_cast<EngineTimer*>(h->data);
        WTFTimer__fire(t->owner);
      },
      ms, repeat ? ms : 0);
}

void WTFTimer__deinit(EngineTimer* t) {
  uv_timer_stop(&t->timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&t->timer),
           [](uv_handle_t* h) { delete static_cast<EngineTimer*>(h->data); });
}

bool WTFTimer__isActive(const EngineTimer* t) {
  return uv_is_active(reinterpret_cast<const uv_handle_t*>(&t->timer));
}

double WTFTimer__secondsUntilTimer(const EngineTimer* t) {
  return static_cast<double>(uv_timer_get_due_in(&t->timer)) / 1000.0;
}

void WTFTimer__cancel(EngineTimer* t) {
  uv_timer_stop(&t->timer);
}

void Bun__errorInstance__finalize(void*) {
  std::abort();
}

void Bun__reportUnhandledError(void*, std::int64_t) {
  Abomin::Runtime::current().report("Uncaught exception in microtask");
}
}
