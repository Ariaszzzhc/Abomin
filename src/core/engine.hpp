#pragma once
#include <JavaScriptCore/JavaScript.h>

#include <atomic>
#include <memory>
#include <string>

namespace Abomin {
  /** One VM on one owning thread. 一个 VM 仅在所属线程执行。 */
  class Engine {
  public:
    explicit Engine(std::atomic_bool& stopped);
    ~Engine();
    Engine(const Engine&) = delete;
    JSGlobalContextRef context() const;
    JSValueRef evaluate(std::string_view source, std::string_view name);
    JSObjectRef import(const std::string& url);
    void checkpoint();
    void interrupt();

  private:
    struct State;
    std::unique_ptr<State> state_;
  };
}  // namespace Abomin
