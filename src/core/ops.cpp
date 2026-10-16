#include <JavaScriptCore/JSContextRefPrivate.h>
#include <ada.h>
#include <fcntl.h>
#include <simdutf.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "core/compute_pool.hpp"
#include "core/module_loader.hpp"
#include "core/runtime.hpp"
#include "core/value.hpp"
#include "services/ffi.hpp"

namespace Abomin {
  namespace {
    using Args = std::span<const JSValueRef>;
    using Op = JSValueRef (*)(Runtime&, Args);

    struct OpEntry {
      const char* name;
      unsigned minimum;
      Op callback;
    };

    std::mutex printMutex;

    std::uint32_t id(Runtime& r, JSValueRef value) {
      auto n = number(r.context(), value);
      if (n < 0 || n > UINT32_MAX || std::floor(n) != n)
        throw std::runtime_error("Expected a non-negative 32-bit integer");
      return static_cast<std::uint32_t>(n);
    }

    JSValueRef take(Runtime& r, AbBuffer result, bool asBytes) {
      std::string message;
      JSValueRef value = nullptr;
      if (result.error)
        message.assign(reinterpret_cast<char*>(result.data), result.len);
      else if (asBytes)
        value = buffer(r.context(), {result.data, result.len});
      else
        value = string(r.context(), {reinterpret_cast<char*>(result.data), result.len});
      ab_buffer_free(result);
      if (!message.empty()) throw std::runtime_error(message);
      return value;
    }

    struct FsRequest {
      uv_fs_t request{};
      Runtime* runtime{};
      std::uint32_t call{};

      enum Kind { Open, Read, Write, Stat, Remove, Mkdir } kind;

      std::shared_ptr<FileResource> file;
      std::vector<std::uint8_t> storage;
      uv_buf_t buf{};

      static void done(uv_fs_t* req) {
        std::unique_ptr<FsRequest> task(static_cast<FsRequest*>(req->data));
        auto& r = *task->runtime;
        auto result = req->result;
        auto st = req->statbuf;
        uv_fs_req_cleanup(req);
        if (result < 0) {
          auto message = std::string(uv_strerror(static_cast<int>(result)));
          r.complete(task->call, false, [&r, message] { return error(r.context(), message); });
        } else {
          auto kind = task->kind;
          auto data = std::move(task->storage);
          if (kind == Open) result = r.addFile(static_cast<uv_file>(result));
          r.complete(task->call, true, [&r, result, kind, data = std::move(data), st] {
            if (kind == Read)
              return static_cast<JSValueRef>(
                  buffer(r.context(), {data.data(), static_cast<std::size_t>(result)}));
            if (kind == Stat) {
              auto object = JSObjectMake(r.context(), nullptr, nullptr);
              property(r.context(), object, "size", JSValueMakeNumber(r.context(), st.st_size));
              property(r.context(), object, "isFile",
                       JSValueMakeBoolean(r.context(), (st.st_mode & S_IFMT) == S_IFREG));
              property(r.context(), object, "isDirectory",
                       JSValueMakeBoolean(r.context(), (st.st_mode & S_IFMT) == S_IFDIR));
              return static_cast<JSValueRef>(object);
            }
            return JSValueMakeNumber(r.context(), static_cast<double>(result));
          });
        }
      }

      void scheduled(int code) {
        if (code < 0) {
          request.result = code;
          done(&request);
        }
      }
    };

    FsRequest* fs(Runtime& r, JSValueRef call, FsRequest::Kind kind) {
      auto* req = new FsRequest{};
      req->runtime = &r;
      req->call = id(r, call);
      req->kind = kind;
      req->request.data = req;
      return req;
    }

    JSValueRef undefined(Runtime& r) {
      return JSValueMakeUndefined(r.context());
    }

    JSValueRef print(Runtime& r, Args a) {
      auto text = Abomin::text(r.context(), a[0]);
      std::lock_guard lock(printMutex);
      auto* stream = a.size() > 1 && JSValueToBoolean(r.context(), a[1]) ? stderr : stdout;
      std::fwrite(text.data(), 1, text.size(), stream);
      std::fputc('\n', stream);
      std::fflush(stream);
      return undefined(r);
    }

    JSValueRef open(Runtime& r, Args a) {
      auto path = text(r.context(), a[1]);
      auto mode = text(r.context(), a[2]);
      int flags = mode == "r"   ? O_RDONLY
                  : mode == "w" ? O_WRONLY | O_CREAT | O_TRUNC
                  : mode == "a" ? O_WRONLY | O_CREAT | O_APPEND
                                : -1;
      if (flags < 0) throw std::runtime_error("Unsupported file mode");
#ifdef _WIN32
      flags |= O_BINARY;
#endif
      auto* req = fs(r, a[0], FsRequest::Open);
      req->scheduled(
          uv_fs_open(r.loop(), &req->request, path.c_str(), flags, 0666, FsRequest::done));
      return undefined(r);
    }

    JSValueRef read(Runtime& r, Args a) {
      auto size = id(r, a[2]);
      if (size > 16 * 1024 * 1024) throw std::runtime_error("Read exceeds 16 MiB limit");
      auto file = r.file(id(r, a[1]));
      auto* req = fs(r, a[0], FsRequest::Read);
      req->file = std::move(file);
      req->storage.resize(size);
      req->buf = uv_buf_init(reinterpret_cast<char*>(req->storage.data()), size);
      req->scheduled(
          uv_fs_read(r.loop(), &req->request, req->file->fd, &req->buf, 1, -1, FsRequest::done));
      return undefined(r);
    }

    JSValueRef write(Runtime& r, Args a) {
      auto file = r.file(id(r, a[1]));
      auto input = bytes(r.context(), a[2]);
      if (input.size() > 16 * 1024 * 1024) throw std::runtime_error("Write exceeds 16 MiB limit");
      auto* req = fs(r, a[0], FsRequest::Write);
      req->file = std::move(file);
      req->storage.assign(input.begin(), input.end());
      req->buf = uv_buf_init(reinterpret_cast<char*>(req->storage.data()),
                             static_cast<unsigned>(input.size()));
      req->scheduled(
          uv_fs_write(r.loop(), &req->request, req->file->fd, &req->buf, 1, -1, FsRequest::done));
      return undefined(r);
    }

    JSValueRef stat(Runtime& r, Args a) {
      auto path = text(r.context(), a[1]);
      auto* req = fs(r, a[0], FsRequest::Stat);
      req->scheduled(uv_fs_stat(r.loop(), &req->request, path.c_str(), FsRequest::done));
      return undefined(r);
    }

    JSValueRef remove(Runtime& r, Args a) {
      auto path = text(r.context(), a[1]);
      auto* req = fs(r, a[0], FsRequest::Remove);
      req->scheduled(uv_fs_unlink(r.loop(), &req->request, path.c_str(), FsRequest::done));
      return undefined(r);
    }

    JSValueRef mkdir(Runtime& r, Args a) {
      auto path = text(r.context(), a[1]);
      auto* req = fs(r, a[0], FsRequest::Mkdir);
      req->scheduled(uv_fs_mkdir(r.loop(), &req->request, path.c_str(), 0777, FsRequest::done));
      return undefined(r);
    }

    JSValueRef url(Runtime& r, Args a) {
      auto raw = text(r.context(), a[0]);
      auto base = a.size() > 1 && !JSValueIsUndefined(r.context(), a[1]) ? text(r.context(), a[1])
                                                                         : std::string{};
      auto parsedBase = ada::parse<ada::url_aggregator>(base);
      auto parsed = ada::parse<ada::url_aggregator>(raw, parsedBase ? &*parsedBase : nullptr);
      if (!parsed) throw std::runtime_error("Invalid URL");
      if (a.size() > 3) {
        auto key = text(r.context(), a[2]), value = text(r.context(), a[3]);
        bool valid = true;
        if (key == "protocol")
          valid = parsed->set_protocol(value);
        else if (key == "hostname")
          valid = parsed->set_hostname(value);
        else if (key == "host")
          valid = parsed->set_host(value);
        else if (key == "port")
          valid = parsed->set_port(value);
        else if (key == "username")
          valid = parsed->set_username(value);
        else if (key == "password")
          valid = parsed->set_password(value);
        else if (key == "pathname")
          valid = parsed->set_pathname(value);
        else if (key == "search")
          parsed->set_search(value);
        else if (key == "hash")
          parsed->set_hash(value);
        if (!valid) return JSValueMakeNull(r.context());
      }
      auto result = JSObjectMake(r.context(), nullptr, nullptr);
      auto put = [&](std::string_view name, auto value) {
        property(r.context(), result, name, string(r.context(), value));
      };
      put("href", parsed->get_href());
      put("origin", parsed->get_origin());
      put("protocol", parsed->get_protocol());
      put("username", parsed->get_username());
      put("password", parsed->get_password());
      put("host", parsed->get_host());
      put("hostname", parsed->get_hostname());
      put("port", parsed->get_port());
      put("pathname", parsed->get_pathname());
      put("search", parsed->get_search());
      put("hash", parsed->get_hash());
      return result;
    }

    JSValueRef encode(Runtime& r, Args a) {
      auto object = JSValueToObject(r.context(), a[0], nullptr);
      auto source = JSValueToStringCopy(r.context(), object, nullptr);
      if (!source) throw std::runtime_error("Expected text");
      auto chars = JSStringGetCharactersPtr(source);
      auto length = JSStringGetLength(source);
      std::vector<std::uint8_t> output(length * 3);
      auto count = simdutf::convert_utf16_to_utf8_with_errors(
          reinterpret_cast<const char16_t*>(chars), length, reinterpret_cast<char*>(output.data()));
      if (count.error != simdutf::error_code::SUCCESS) {
        // JavaScript strings may contain lone surrogates. JS 字符串允许孤立代理项，编码时替换。
        std::u16string clean(reinterpret_cast<const char16_t*>(chars), length);
        for (std::size_t i = 0; i < length; ++i) {
          auto c = clean[i];
          if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 < length && clean[i + 1] >= 0xdc00 && clean[i + 1] <= 0xdfff)
              ++i;
            else
              clean[i] = 0xfffd;
          } else if (c >= 0xdc00 && c <= 0xdfff)
            clean[i] = 0xfffd;
        }
        count = simdutf::convert_utf16_to_utf8_with_errors(clean.data(), length,
                                                           reinterpret_cast<char*>(output.data()));
      }
      JSStringRelease(source);
      output.resize(count.count);
      return buffer(r.context(), output);
    }

    JSValueRef decode(Runtime& r, Args a) {
      auto input = bytes(r.context(), a[0]);
      auto label = text(r.context(), a[1]);
      return take(r,
                  ab_decode(reinterpret_cast<const std::uint8_t*>(label.data()), label.size(),
                            input.data(), input.size(), JSValueToBoolean(r.context(), a[2])),
                  false);
    }

    JSValueRef base64(Runtime& r, Args a) {
      auto input = bytes(r.context(), a[0]);
      return take(r, ab_base64(input.data(), input.size(), JSValueToBoolean(r.context(), a[1])),
                  true);
    }

    JSValueRef random(Runtime& r, Args a) {
      auto input = bytes(r.context(), a[0]);
      if (input.size() > 65536) throw std::runtime_error("Random buffer exceeds 65536 bytes");
      if (!ab_random(input.data(), input.size()))
        throw std::runtime_error("Random number generation failed");
      return a[0];
    }

    JSValueRef digest(Runtime& r, Args a) {
      auto input = bytes(r.context(), a[1]);
      return take(r, ab_digest(id(r, a[0]), input.data(), input.size()), true);
    }

    JSValueRef digestAsync(Runtime& r, Args a) {
      auto call = id(r, a[0]);
      auto algorithm = id(r, a[1]);
      auto input = bytes(r.context(), a[2]);
      if (input.size() > 64 * 1024 * 1024) throw std::runtime_error("Digest input exceeds 64 MiB");
      std::vector<std::uint8_t> copy(input.begin(), input.end());
      auto inbox = r.inbox();
      r.retain();
      if (!ComputePool::shared().submit([&r, inbox, call, algorithm, copy = std::move(copy)] {
            auto result = ab_digest(algorithm, copy.data(), copy.size());
            auto data = std::make_shared<std::vector<std::uint8_t>>(result.data,
                                                                    result.data + result.len);
            bool failed = result.error;
            ab_buffer_free(result);
            inbox->post([&r, call, data, failed] {
              r.complete(call, !failed, [&r, data, failed] {
                return failed ? error(r.context(),
                                      {reinterpret_cast<const char*>(data->data()), data->size()})
                              : static_cast<JSValueRef>(buffer(r.context(), *data));
              });
              r.release();
            });
          })) {
        r.release();
        throw std::runtime_error("Native computation queue is full");
      }
      return undefined(r);
    }

    JSValueRef config(Runtime& r, Args) {
      auto result = JSObjectMake(r.context(), nullptr, nullptr);
      std::vector<JSValueRef> argv;
      for (const auto& arg : r.args) {
        auto value = string(r.context(), arg);
        JSValueProtect(r.context(), value);
        argv.push_back(value);
      }
      property(r.context(), result, "args",
               JSObjectMakeArray(r.context(), argv.size(), argv.data(), nullptr));
      for (auto value : argv) JSValueUnprotect(r.context(), value);
      property(r.context(), result, "entry", string(r.context(), r.entry));
      property(r.context(), result, "worker", JSValueMakeBoolean(r.context(), r.isWorker()));
      property(r.context(), result, "version", string(r.context(), "0.1.0"));
      return result;
    }

    JSValueRef invoke(JSContextRef ctx, JSObjectRef function, JSObjectRef, std::size_t count,
                      const JSValueRef values[], JSValueRef* exception) {
      auto* op = static_cast<const OpEntry*>(JSObjectGetPrivate(function));
      try {
        if (count < op->minimum)
          throw std::runtime_error(std::string(op->name) + ": missing argument");
        return op->callback(Runtime::current(), {values, count});
      } catch (const std::exception& e) {
        *exception = error(ctx, e.what());
        return JSValueMakeUndefined(ctx);
      }
    }
  }  // namespace

  void installNetworkOps(Runtime&, JSObjectRef);
  void installWebOps(Runtime&, JSObjectRef);
  void installPortOps(Runtime&, JSObjectRef);

  void installOps(Runtime& r) {
    static const OpEntry ops[] = {
        {"retain", 0,
         [](Runtime& r, Args) {
           r.retain();
           return undefined(r);
         }},
        {"release", 0,
         [](Runtime& r, Args) {
           r.release();
           return undefined(r);
         }},
        {"setUnhandled", 1,
         [](Runtime& r, Args a) {
           JSValueRef e = nullptr;
           JSGlobalContextSetUnhandledRejectionCallback(JSContextGetGlobalContext(r.context()),
                                                        JSValueToObject(r.context(), a[0], &e), &e);
           if (e) throw std::runtime_error(text(r.context(), e));
           return undefined(r);
         }},
        {"digestAsync", 3, digestAsync},
        {"print", 1, print},
        {"config", 0, config},
        {"setDispatcher", 1,
         [](Runtime& r, Args a) {
           r.setDispatcher(JSValueToObject(r.context(), a[0], nullptr));
           return undefined(r);
         }},
        {"timer", 2,
         [](Runtime& r, Args a) {
           auto n = number(r.context(), a[0]);
           if (!std::isfinite(n)) n = 1;
           return JSValueMakeNumber(r.context(), r.timer(n, JSValueToBoolean(r.context(), a[1])));
         }},
        {"cancelTimer", 1,
         [](Runtime& r, Args a) {
           r.cancelTimer(id(r, a[0]));
           return undefined(r);
         }},
        {"open", 3, open},
        {"read", 3, read},
        {"write", 3, write},
        {"stat", 2, stat},
        {"remove", 2, remove},
        {"mkdir", 2, mkdir},
        {"closeFile", 1,
         [](Runtime& r, Args a) {
           r.closeFile(id(r, a[0]));
           return undefined(r);
         }},
        {"url", 1, url},
        {"encode", 1, encode},
        {"decode", 3, decode},
        {"base64", 2, base64},
        {"random", 1, random},
        {"digest", 2, digest},
        {"now", 0,
         [](Runtime& r, Args) {
           return JSValueMakeNumber(r.context(), static_cast<double>(uv_hrtime()) / 1e6);
         }},
        {"cwd", 0,
         [](Runtime& r, Args) {
           return string(r.context(), std::filesystem::current_path().string());
         }},
        {"env", 1,
         [](Runtime& r, Args a) {
           auto key = text(r.context(), a[0]);
           auto* v = std::getenv(key.c_str());
           return v ? string(r.context(), v) : JSValueMakeUndefined(r.context());
         }},
        {"exit", 1,
         [](Runtime& r, Args a) {
           r.exitCode = static_cast<int>(id(r, a[0]) & 255);
           r.requestStop();
           return undefined(r);
         }},
        {"report", 1,
         [](Runtime& r, Args a) {
           r.report(text(r.context(), a[0]));
           return undefined(r);
         }},
        {"worker", 1,
         [](Runtime& r, Args a) {
           return JSValueMakeNumber(r.context(), r.worker(ModuleLoader::resolve(
                                                     text(r.context(), a[0]), r.entry, false)));
         }},
        {"postWorker", 2,
         [](Runtime& r, Args a) {
           r.postWorker(id(r, a[0]), text(r.context(), a[1]));
           return undefined(r);
         }},
        {"terminateWorker", 1,
         [](Runtime& r, Args a) {
           r.terminateWorker(id(r, a[0]));
           return undefined(r);
         }},
        {"postParent", 1,
         [](Runtime& r, Args a) {
           r.postParent(text(r.context(), a[0]));
           return undefined(r);
         }},
        {"close", 0, [](Runtime& r, Args) {
           r.requestStop();
           return undefined(r);
         }}};
    JSClassDefinition definition = kJSClassDefinitionEmpty;
    definition.className = "NativeOp";
    definition.callAsFunction = invoke;
    auto cls = JSClassCreate(&definition);
    auto native = JSObjectMake(r.context(), nullptr, nullptr);
    for (auto& op : ops)
      property(r.context(), native, op.name,
               JSObjectMake(r.context(), cls, const_cast<OpEntry*>(&op)));
    JSClassRelease(cls);
    installNetworkOps(r, native);
    installPortOps(r, native);
    installWebOps(r, native);
    property(r.context(), JSContextGetGlobalObject(r.context()), "__native", native);
  }
}  // namespace Abomin
