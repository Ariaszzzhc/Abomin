#pragma once
#include <JavaScriptCore/JavaScript.h>
#include <simdutf.h>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Abomin {
  /** RAII for JSC strings. JSC 字符串的自动生命周期管理。 */
  class String {
  public:
    explicit String(std::string_view s) {
      std::u16string utf16(simdutf::utf16_length_from_utf8(s.data(), s.size()), u'\0');
      auto size = simdutf::convert_utf8_to_utf16(s.data(), s.size(), utf16.data());
      value_ = JSStringCreateWithCharacters(reinterpret_cast<const JSChar*>(utf16.data()), size);
    }

    ~String() {
      JSStringRelease(value_);
    }

    String(const String&) = delete;

    operator JSStringRef() const {
      return value_;
    }

  private:
    JSStringRef value_;
  };

  inline std::string text(JSContextRef ctx, JSValueRef value) {
    JSValueRef error = nullptr;
    auto s = JSValueToStringCopy(ctx, value, &error);
    if (!s || error) throw std::runtime_error("Cannot convert value to string");
    std::string out(JSStringGetMaximumUTF8CStringSize(s), '\0');
    auto size = JSStringGetUTF8CString(s, out.data(), out.size());
    JSStringRelease(s);
    out.resize(size ? size - 1 : 0);
    return out;
  }

  inline JSValueRef string(JSContextRef ctx, std::string_view value) {
    return JSValueMakeString(ctx, String(value));
  }

  inline JSValueRef property(JSContextRef ctx, JSObjectRef object, std::string_view name) {
    JSValueRef error = nullptr;
    auto value = JSObjectGetProperty(ctx, object, String(name), &error);
    if (error) throw std::runtime_error(text(ctx, error));
    return value;
  }

  inline void property(JSContextRef ctx, JSObjectRef object, std::string_view name,
                       JSValueRef value) {
    JSValueRef error = nullptr;
    JSObjectSetProperty(ctx, object, String(name), value, kJSPropertyAttributeNone, &error);
    if (error) throw std::runtime_error(text(ctx, error));
  }

  inline JSValueRef error(JSContextRef ctx, std::string_view message) {
    auto value = string(ctx, message);
    return JSObjectMakeError(ctx, 1, &value, nullptr);
  }

  inline double number(JSContextRef ctx, JSValueRef value) {
    if (!JSValueIsNumber(ctx, value)) throw std::runtime_error("Expected a number");
    return JSValueToNumber(ctx, value, nullptr);
  }

  /** Borrow until the next allocating VM call; async operations must copy first.
   * 借用仅持续到下次 VM 分配调用；异步操作必须预先复制。 */
  std::span<std::uint8_t> bytes(JSContextRef ctx, JSValueRef value);

  inline JSObjectRef buffer(JSContextRef ctx, std::span<const std::uint8_t> data) {
    auto* copy = new std::uint8_t[data.size() ? data.size() : 1];
    std::copy(data.begin(), data.end(), copy);
    return JSObjectMakeArrayBufferWithBytesNoCopy(
        ctx, copy, data.size(), [](void* p, void*) { delete[] static_cast<std::uint8_t*>(p); },
        nullptr, nullptr);
  }

  inline JSValueRef call(JSContextRef ctx, JSObjectRef fn, std::span<const JSValueRef> args = {}) {
    JSValueRef exception = nullptr;
    auto result = JSObjectCallAsFunction(ctx, fn, nullptr, args.size(), args.data(), &exception);
    if (exception) throw std::runtime_error(text(ctx, exception));
    return result;
  }
}  // namespace Abomin
