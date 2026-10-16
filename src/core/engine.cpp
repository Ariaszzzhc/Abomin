// clang-format off
#include "cmakeconfig.h"
// clang-format on
#include "core/engine.hpp"

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/GlobalObjectMethodTable.h>
#include <JavaScriptCore/JSArrayBuffer.h>
#include <JavaScriptCore/JSArrayBufferView.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSContextRefPrivate.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/JSSourceCode.h>
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/SourceCode.h>

#include "core/module_loader.hpp"
#include "core/value.hpp"

/** Linux engine binaries use GCC 13's C++ standard-library ABI.
 * Linux 引擎预编译包使用 GCC 13 的 C++ 标准库 ABI。 */
#if defined(__linux__) && (!defined(_GLIBCXX_RELEASE) || _GLIBCXX_RELEASE != 13)
#  error "The pinned Linux JavaScriptCore SDK requires GCC 13 libstdc++ headers"
#endif

namespace Abomin {
  namespace {
    JSC::Identifier resolve(JSC::JSGlobalObject* global, JSC::JSModuleLoader*, JSC::JSValue name,
                            JSC::JSValue referrer, RefPtr<JSC::ScriptFetcher>, bool) {
      auto& vm = global->vm();
      try {
        auto key = name.toWTFString(global).utf8();
        auto parent = referrer.isUndefined()
                          ? std::string{}
                          : std::string(referrer.toWTFString(global).utf8().data());
        return JSC::Identifier::fromString(
            vm, WTF::String::fromUTF8(ModuleLoader::resolve(key.data(), parent)));
      } catch (const std::exception& e) {
        auto scope = DECLARE_THROW_SCOPE(vm);
        JSC::throwException(global, scope,
                            JSC::createError(global, WTF::String::fromUTF8(e.what())));
        return {};
      }
    }

    JSC::JSPromise* fetch(JSC::JSGlobalObject* global, JSC::JSModuleLoader*, JSC::JSValue key,
                          RefPtr<JSC::ScriptFetchParameters>, RefPtr<JSC::ScriptFetcher>) {
      auto& vm = global->vm();
      auto* promise = JSC::JSPromise::create(vm, global->promiseStructure());
      try {
        auto url = key.toWTFString(global);
        auto code = ModuleLoader::source(url.utf8().data());
        auto source = JSC::makeSource(
            WTF::String::fromUTF8(code), JSC::SourceOrigin(WTF::URL({}, url)),
            JSC::SourceTaintedOrigin::Untainted, url, {}, JSC::SourceProviderSourceType::Module);
        promise->resolve(global, vm, JSC::JSSourceCode::create(vm, WTF::move(source)));
      } catch (const std::exception& e) {
        promise->reject(vm, JSC::createError(global, WTF::String::fromUTF8(e.what())));
      }
      return promise;
    }

    JSC::JSPromise* dynamicImport(JSC::JSGlobalObject* global, JSC::JSModuleLoader*,
                                  JSC::JSString* name,
                                  RefPtr<JSC::ScriptFetchParameters> parameters,
                                  const JSC::SourceOrigin& origin, bool deferred) {
      return JSC::importModule(global,
                               JSC::Identifier::fromString(global->vm(), name->value(global)),
                               JSC::Identifier::fromString(global->vm(), origin.url().string()),
                               WTF::move(parameters), nullptr, deferred);
    }

    JSC::JSObject* meta(JSC::JSGlobalObject* global, JSC::JSModuleLoader*, JSC::JSValue key,
                        JSC::JSModuleRecord*, RefPtr<JSC::ScriptFetcher>) {
      auto* object = JSC::constructEmptyObject(global);
      object->putDirect(global->vm(), JSC::Identifier::fromString(global->vm(), "url"), key);
      return object;
    }
  }  // namespace

  std::span<std::uint8_t> bytes(JSContextRef ctx, JSValueRef value) {
    if (!JSValueIsObject(ctx, value)) throw std::runtime_error("Expected BufferSource");
    auto* object = toJS(JSValueToObject(ctx, value, nullptr));
    if (auto* array = dynamicDowncast<JSC::JSArrayBufferView>(object)) {
      if (array->isDetached()) throw std::runtime_error("Detached buffer");
      return {static_cast<std::uint8_t*>(array->vector()), array->byteLength()};
    }
    if (auto* array = dynamicDowncast<JSC::JSArrayBuffer>(object)) {
      if (array->impl()->isDetached()) throw std::runtime_error("Detached buffer");
      return array->impl()->mutableSpan();
    }
    throw std::runtime_error("Expected BufferSource");
  }

  struct Engine::State {
    JSContextGroupRef group{};
    JSGlobalContextRef ctx{};
    JSC::GlobalObjectMethodTable methods{};
  };

  Engine::Engine(std::atomic_bool& stopped) : state_(std::make_unique<State>()) {
    state_->group = JSContextGroupCreate();
    auto temporary = JSGlobalContextCreateInGroup(state_->group, nullptr);
    auto* global = toJS(temporary);
    auto& vm = global->vm();
    JSC::JSLockHolder lock(vm);
    state_->methods = *global->globalObjectMethodTable();
    state_->methods.moduleLoaderResolve = resolve;
    state_->methods.moduleLoaderFetch = fetch;
    state_->methods.moduleLoaderImportModule = dynamicImport;
    state_->methods.moduleLoaderCreateImportMetaProperties = meta;
    auto* custom = JSC::JSGlobalObject::createWithCustomMethodTable(
        vm, JSC::JSGlobalObject::createStructure(vm, JSC::jsNull()), &state_->methods);
    state_->ctx = JSGlobalContextRetain(toGlobalRef(custom));
    JSGlobalContextRelease(temporary);
    JSContextGroupSetExecutionTimeLimit(
        state_->group, 0.025,
        [](JSContextRef, void* flag) { return static_cast<std::atomic_bool*>(flag)->load(); },
        &stopped);
  }

  Engine::~Engine() {
    JSContextGroupClearExecutionTimeLimit(state_->group);
    JSGlobalContextRelease(state_->ctx);
    JSContextGroupRelease(state_->group);
  }

  JSGlobalContextRef Engine::context() const {
    return state_->ctx;
  }

  JSValueRef Engine::evaluate(std::string_view source, std::string_view name) {
    JSValueRef exception = nullptr;
    auto result = JSEvaluateScript(context(), String(source), nullptr, String(name), 1, &exception);
    if (exception) throw std::runtime_error(text(context(), exception));
    return result;
  }

  JSObjectRef Engine::import(const std::string& url) {
    auto* global = toJS(context());
    JSC::JSLockHolder lock(global->vm());
    return toRef(JSC::loadAndEvaluateModule(global, WTF::String::fromUTF8(url), nullptr, nullptr));
  }

  void Engine::interrupt() {
    toJS(context())->vm().notifyNeedTermination();
  }

  void Engine::checkpoint() {
    auto* global = toJS(context());
    JSC::JSLockHolder lock(global->vm());
    global->vm().drainMicrotasks();
  }
}  // namespace Abomin
