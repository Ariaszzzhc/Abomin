#include <array>
#include <cmath>
#include <mutex>

#include "core/runtime.hpp"
#include "core/value.hpp"

namespace Abomin {
  namespace {
    struct Endpoint {
      std::uint32_t peer;
      std::weak_ptr<Inbox> owner;
      std::deque<std::string> queue;
    };

    std::mutex mutex;
    std::unordered_map<std::uint32_t, Endpoint> endpoints;
    std::uint32_t nextPort{1};

    std::uint32_t portId(JSContextRef ctx, JSValueRef value) {
      auto n = number(ctx, value);
      if (n < 1 || n > UINT32_MAX || std::floor(n) != n) throw std::runtime_error("Invalid port");
      return static_cast<std::uint32_t>(n);
    }

    JSValueRef create(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t, const JSValueRef[],
                      JSValueRef*) {
      std::lock_guard lock(mutex);
      auto a = nextPort++, b = nextPort++;
      endpoints.emplace(a, Endpoint{b, Runtime::current().inbox(), {}});
      endpoints.emplace(b, Endpoint{a, Runtime::current().inbox(), {}});
      JSValueRef values[] = {JSValueMakeNumber(ctx, a), JSValueMakeNumber(ctx, b)};
      return JSObjectMakeArray(ctx, 2, values, nullptr);
    }

    JSValueRef operate(JSContextRef ctx, JSObjectRef fn, JSObjectRef, std::size_t count,
                       const JSValueRef values[], JSValueRef* exception) {
      try {
        if (count < 2) throw std::runtime_error("Missing port argument");
        auto action = text(ctx, values[0]);
        auto id = portId(ctx, values[1]);
        std::lock_guard lock(mutex);
        auto it = endpoints.find(id);
        if (it == endpoints.end()) throw std::runtime_error("Closed message port");
        auto& port = it->second;
        if (action == "attach") {
          if (auto owner = port.owner.lock(); owner && owner != Runtime::current().inbox())
            throw std::runtime_error("Port belongs to another worker");
          port.owner = Runtime::current().inbox();
          if (!port.queue.empty()) Runtime::current().inbox()->send({Envelope::PortReady, id, {}});
        } else {
          if (port.owner.lock() != Runtime::current().inbox())
            throw std::runtime_error("Port is detached");
          if (action == "close")
            endpoints.erase(it);
          else if (action == "disown")
            port.owner.reset();
          else if (action == "post") {
            if (count < 3) throw std::runtime_error("Missing message");
            if (auto peer = endpoints.find(port.peer); peer != endpoints.end()) {
              if (peer->second.queue.size() >= 1024)
                throw std::runtime_error("MessagePort queue capacity exceeded");
              peer->second.queue.push_back(text(ctx, values[2]));
              if (auto owner = peer->second.owner.lock())
                owner->send({Envelope::PortReady, peer->first, {}});
            }
          } else
            throw std::runtime_error("Unknown port operation");
        }
        return JSValueMakeUndefined(ctx);
      } catch (const std::exception& e) {
        *exception = error(ctx, e.what());
        return JSValueMakeUndefined(ctx);
      }
    }
  }  // namespace

  void deliverPort(Runtime& r, std::uint32_t id) {
    std::deque<std::string> incoming;
    {
      std::lock_guard lock(mutex);
      auto it = endpoints.find(id);
      if (it == endpoints.end() || it->second.owner.lock() != r.inbox()) return;
      incoming.swap(it->second.queue);
    }
    for (auto& packet : incoming) r.dispatch("port", id, string(r.context(), packet));
  }

  void closePorts(const std::shared_ptr<Inbox>& inbox) {
    std::lock_guard lock(mutex);
    std::erase_if(endpoints, [&](const auto& item) { return item.second.owner.lock() == inbox; });
  }

  void installPortOps(Runtime& r, JSObjectRef native) {
    property(r.context(), native, "portCreate",
             JSObjectMakeFunctionWithCallback(r.context(), String("portCreate"), create));
    property(r.context(), native, "port",
             JSObjectMakeFunctionWithCallback(r.context(), String("port"), operate));
  }
}  // namespace Abomin
