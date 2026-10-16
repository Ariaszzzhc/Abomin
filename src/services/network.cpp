#include <ada.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

#include "core/runtime.hpp"
#include "core/value.hpp"
#include "services/ffi.hpp"

namespace Abomin {
  namespace {
    struct Session;

    struct Tcp {
      uv_tcp_t handle{};
      std::shared_ptr<Session> session;
    };

    struct Lookup {
      uv_getaddrinfo_t request{};
      std::shared_ptr<Session> session;
    };

    struct Connect {
      uv_connect_t request{};
      std::shared_ptr<Session> session;
    };

    struct Write {
      uv_write_t request{};
      std::shared_ptr<Session> session;
      std::vector<char> bytes;
    };

    thread_local std::unordered_map<std::uint32_t, std::shared_ptr<Session>> sessions;
    thread_local std::uint32_t nextSession{1};

    struct Session : std::enable_shared_from_this<Session> {
      Runtime* runtime{};
      std::uint32_t rid{}, call{};
      Tcp* tcp{};
      Lookup* lookup{};
      std::vector<sockaddr_storage> addresses;
      std::string url, method, host, port, failure;
      std::vector<std::pair<std::string, std::string>> headers;
      std::vector<std::uint8_t> requestBody, incoming;
      std::size_t offset{}, addressIndex{};
      void* task{};
      std::optional<std::uint32_t> readCall;
      bool retained{}, closed{}, connected{}, reading{}, writing{}, eof{}, head{};

      ~Session() {
        if (task) ab_http_free(task);
      }

      void close() {
        if (closed) return;
        closed = true;
        if (retained) {
          retained = false;
          runtime->release();
        }
        if (readCall) {
          readCall.reset();
          runtime->release();
        }
        if (task) {
          ab_http_free(task);
          task = nullptr;
        }
        if (lookup) uv_cancel(reinterpret_cast<uv_req_t*>(&lookup->request));
        if (tcp) {
          auto* h = tcp;
          tcp = nullptr;
          uv_read_stop(reinterpret_cast<uv_stream_t*>(&h->handle));
          uv_close(reinterpret_cast<uv_handle_t*>(&h->handle),
                   [](uv_handle_t* h) { delete reinterpret_cast<Tcp*>(h); });
        }
      }

      void fail(std::string message) {
        failure = std::move(message);
        if (!head)
          runtime->complete(call, false, [r = runtime, message = failure] {
            return error(r->context(), message);
          });
        if (readCall) {
          runtime->complete(*readCall, false, [r = runtime, message = failure] {
            return error(r->context(), message);
          });
          readCall.reset();
          runtime->release();
        }
        close();
      }

      void connect() {
        if (closed) return;
        if (addressIndex == addresses.size()) {
          fail("Cannot connect to " + host + ":" + port);
          return;
        }
        tcp = new Tcp{};
        tcp->session = shared_from_this();
        uv_tcp_init(runtime->loop(), &tcp->handle);
        tcp->handle.data = this;
        auto* request = new Connect{};
        request->session = shared_from_this();
        request->request.data = request;
        auto status = uv_tcp_connect(
            &request->request, &tcp->handle,
            reinterpret_cast<sockaddr*>(&addresses[addressIndex++]),
            [](uv_connect_t* req, int status) {
              std::unique_ptr<Connect> request(static_cast<Connect*>(req->data));
              auto s = request->session;
              if (s->closed) return;
              if (status < 0) {
                auto* old = s->tcp;
                s->tcp = nullptr;
                uv_close(reinterpret_cast<uv_handle_t*>(&old->handle), [](uv_handle_t* h) {
                  auto s = reinterpret_cast<Tcp*>(h)->session;
                  delete reinterpret_cast<Tcp*>(h);
                  s->connect();
                });
              } else
                s->start();
            });
        if (status < 0) {
          delete request;
          fail(uv_strerror(status));
        }
      }

      void start() {
        connected = true;
        std::vector<AbHeader> entries;
        for (const auto& [name, value] : headers)
          entries.push_back({reinterpret_cast<const std::uint8_t*>(name.data()), name.size(),
                             reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        task = ab_http_new(this, reinterpret_cast<const std::uint8_t*>(url.data()), url.size(),
                           reinterpret_cast<const std::uint8_t*>(method.data()), method.size(),
                           entries.data(), entries.size(), requestBody.data(), requestBody.size());
        requestBody.clear();
        runtime->inbox()->send({Envelope::NetworkReady, 0, {}});
      }

      void startRead() {
        if (closed || reading || eof || !tcp) return;
        reading = true;
        auto status = uv_read_start(
            reinterpret_cast<uv_stream_t*>(&tcp->handle),
            [](uv_handle_t*, std::size_t, uv_buf_t* buf) {
              *buf = uv_buf_init(new char[16384], 16384);
            },
            [](uv_stream_t* stream, ssize_t count, const uv_buf_t* buf) {
              auto* s = static_cast<Session*>(stream->data);
              if (count > 0) {
                s->incoming.assign(reinterpret_cast<std::uint8_t*>(buf->base),
                                   reinterpret_cast<std::uint8_t*>(buf->base) + count);
                s->offset = 0;
              }
              if (count < 0) {
                s->eof = count == UV_EOF;
                if (!s->eof) s->failure = uv_strerror(static_cast<int>(count));
              }
              if (count != 0) {
                uv_read_stop(stream);
                s->reading = false;
              }
              delete[] buf->base;
            });
        if (status < 0) {
          reading = false;
          failure = uv_strerror(status);
        }
      }

      void poll() {
        if (closed || !task) return;
        auto result = ab_http_poll(task);
        if (result < 0) {
          auto err = ab_http_error(task);
          std::string message(reinterpret_cast<char*>(err.data), err.len);
          ab_buffer_free(err);
          fail(message);
          return;
        }
        if (result == 1 && !head) {
          head = true;
          if (retained) {
            retained = false;
            runtime->release();
          }
          auto status = ab_http_status(task);
          std::vector<std::pair<std::string, std::string>> values;
          AbView name{}, value{};
          for (std::size_t i = 0; ab_http_header(task, i, &name, &value); ++i)
            values.emplace_back(std::string(reinterpret_cast<const char*>(name.data), name.len),
                                std::string(reinterpret_cast<const char*>(value.data), value.len));
          runtime->complete(
              call, true, [r = runtime, rid = rid, status, values = std::move(values)] {
                auto object = JSObjectMake(r->context(), nullptr, nullptr);
                std::vector<JSValueRef> headers;
                for (const auto& [key, val] : values) {
                  JSValueRef pair[] = {string(r->context(), key), string(r->context(), val)};
                  auto entry = JSObjectMakeArray(r->context(), 2, pair, nullptr);
                  JSValueProtect(r->context(), entry);
                  headers.push_back(entry);
                }
                property(r->context(), object, "status", JSValueMakeNumber(r->context(), status));
                property(r->context(), object, "rid", JSValueMakeNumber(r->context(), rid));
                property(r->context(), object, "headers",
                         JSObjectMakeArray(r->context(), headers.size(), headers.data(), nullptr));
                for (auto entry : headers) JSValueUnprotect(r->context(), entry);
                return object;
              });
          if (tcp) uv_unref(reinterpret_cast<uv_handle_t*>(&tcp->handle));
        }
        if (readCall && head) {
          auto data = ab_http_read(task);
          if (data.error == 2) return;
          auto id = *readCall;
          readCall.reset();
          runtime->release();
          if (data.error == 1) {
            std::string message(reinterpret_cast<char*>(data.data), data.len);
            ab_buffer_free(data);
            runtime->complete(id, false,
                              [r = runtime, message] { return error(r->context(), message); });
            close();
            return;
          }
          bool end = data.error == 3;
          std::vector<std::uint8_t> chunk;
          if (!end) chunk.assign(data.data, data.data + data.len);
          ab_buffer_free(data);
          runtime->complete(id, true, [r = runtime, end, chunk = std::move(chunk)] {
            return end ? JSValueMakeNull(r->context())
                       : static_cast<JSValueRef>(buffer(r->context(), chunk));
          });
          if (end)
            close();
          else if (tcp)
            uv_unref(reinterpret_cast<uv_handle_t*>(&tcp->handle));
        }
      }
    };

    std::uint32_t integer(JSContextRef ctx, JSValueRef value) {
      auto n = number(ctx, value);
      if (n < 0 || n > UINT32_MAX || std::floor(n) != n)
        throw std::runtime_error("Invalid request id");
      return static_cast<std::uint32_t>(n);
    }

    JSValueRef fetch(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t count,
                     const JSValueRef a[], JSValueRef* exception) {
      try {
        if (count < 5) throw std::runtime_error("Missing fetch arguments");
        auto s = std::make_shared<Session>();
        s->runtime = &Runtime::current();
        s->call = integer(ctx, a[0]);
        s->url = text(ctx, a[1]);
        s->method = text(ctx, a[2]);
        auto url = ada::parse<ada::url_aggregator>(s->url);
        if (!url || (url->get_protocol() != "http:" && url->get_protocol() != "https:"))
          throw std::runtime_error("fetch supports http: and https: URLs");
        s->host = url->get_hostname();
        if (s->host.starts_with('[')) s->host = s->host.substr(1, s->host.size() - 2);
        s->port = url->get_port().empty() ? url->get_protocol() == "https:" ? "443" : "80"
                                          : std::string(url->get_port());
        auto list = JSValueToObject(ctx, a[3], nullptr);
        auto len = integer(ctx, property(ctx, list, "length"));
        if (len > 1024) throw std::runtime_error("Too many headers");
        for (std::uint32_t i = 0; i < len; ++i) {
          auto pair
              = JSValueToObject(ctx, JSObjectGetPropertyAtIndex(ctx, list, i, nullptr), nullptr);
          s->headers.emplace_back(text(ctx, JSObjectGetPropertyAtIndex(ctx, pair, 0, nullptr)),
                                  text(ctx, JSObjectGetPropertyAtIndex(ctx, pair, 1, nullptr)));
        }
        auto data = bytes(ctx, a[4]);
        if (data.size() > 64 * 1024 * 1024) throw std::runtime_error("Upload exceeds 64 MiB");
        s->requestBody.assign(data.begin(), data.end());
        s->rid = nextSession++;
        sessions[s->rid] = s;
        s->runtime->retain();
        s->retained = true;
        auto* lookup = new Lookup{};
        lookup->session = s;
        lookup->request.data = lookup;
        s->lookup = lookup;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        auto result = uv_getaddrinfo(
            s->runtime->loop(), &lookup->request,
            [](uv_getaddrinfo_t* req, int status, addrinfo* info) {
              std::unique_ptr<Lookup> lookup(static_cast<Lookup*>(req->data));
              auto s = lookup->session;
              s->lookup = nullptr;
              if (!s->closed) {
                if (status < 0)
                  s->fail(uv_strerror(status));
                else {
                  for (auto* a = info; a; a = a->ai_next) {
                    sockaddr_storage address{};
                    std::memcpy(&address, a->ai_addr,
                                std::min(sizeof(address), static_cast<size_t>(a->ai_addrlen)));
                    s->addresses.push_back(address);
                  }
                  s->connect();
                }
              }
              if (info) uv_freeaddrinfo(info);
            },
            s->host.c_str(), s->port.c_str(), &hints);
        if (result < 0) {
          s->lookup = nullptr;
          delete lookup;
          s->fail(uv_strerror(result));
        }
        return JSValueMakeNumber(ctx, s->rid);
      } catch (const std::exception& e) {
        *exception = error(ctx, e.what());
        return JSValueMakeUndefined(ctx);
      }
    }

    JSValueRef bodyRead(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t count,
                        const JSValueRef a[], JSValueRef* exception) {
      try {
        if (count < 2) throw std::runtime_error("Missing body arguments");
        auto call = integer(ctx, a[0]), rid = integer(ctx, a[1]);
        auto it = sessions.find(rid);
        if (it == sessions.end() || it->second->closed)
          throw std::runtime_error("Response body is closed");
        auto& s = *it->second;
        if (s.readCall) throw std::runtime_error("Concurrent response body read");
        s.readCall = call;
        s.runtime->retain();
        if (s.tcp) uv_ref(reinterpret_cast<uv_handle_t*>(&s.tcp->handle));
        s.poll();
        return JSValueMakeUndefined(ctx);
      } catch (const std::exception& e) {
        *exception = error(ctx, e.what());
        return JSValueMakeUndefined(ctx);
      }
    }

    JSValueRef cancel(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t count,
                      const JSValueRef a[], JSValueRef* exception) {
      try {
        if (!count) throw std::runtime_error("Missing request id");
        auto rid = integer(ctx, a[0]);
        auto it = sessions.find(rid);
        if (it != sessions.end()) {
          if (!it->second->closed) it->second->fail("Request aborted");
          sessions.erase(it);
        }
        return JSValueMakeUndefined(ctx);
      } catch (const std::exception& e) {
        *exception = error(ctx, e.what());
        return JSValueMakeUndefined(ctx);
      }
    }
  }  // namespace

  void pollNetwork(Runtime&) {
    std::vector<std::shared_ptr<Session>> active;
    for (auto& [id, s] : sessions) active.push_back(s);
    for (auto& s : active) s->poll();
    std::erase_if(sessions, [](const auto& p) { return p.second->closed; });
  }

  void shutdownNetwork(Runtime&) {
    for (auto& [id, s] : sessions) s->close();
    sessions.clear();
  }

  void installNetworkOps(Runtime& r, JSObjectRef native) {
    for (auto [name, fn] :
         std::initializer_list<std::pair<const char*, JSObjectCallAsFunctionCallback>>{
             {"fetch", fetch}, {"bodyRead", bodyRead}, {"cancelFetch", cancel}})
      property(r.context(), native, name,
               JSObjectMakeFunctionWithCallback(r.context(), String(name), fn));
  }
}  // namespace Abomin

extern "C" {
std::ptrdiff_t ab_io_read(void* io, std::uint8_t* data, std::size_t len) {
  auto& s = *static_cast<Abomin::Session*>(io);
  if (s.closed || !s.failure.empty()) return -1;
  auto count = std::min(len, s.incoming.size() - s.offset);
  if (count) {
    std::memcpy(data, s.incoming.data() + s.offset, count);
    s.offset += count;
    return static_cast<std::ptrdiff_t>(count);
  }
  if (s.eof) return 0;
  s.startRead();
  return -2;
}

std::ptrdiff_t ab_io_write(void* io, const std::uint8_t* data, std::size_t len) {
  auto& s = *static_cast<Abomin::Session*>(io);
  if (s.closed || !s.failure.empty() || !s.tcp) return -1;
  if (!len) return 0;
  if (s.writing) return -2;
  auto* write = new Abomin::Write{};
  write->session = s.shared_from_this();
  len = std::min(len, std::size_t(65536));
  write->bytes.assign(data, data + len);
  write->request.data = write;
  auto buf = uv_buf_init(write->bytes.data(), static_cast<unsigned>(len));
  s.writing = true;
  auto status
      = uv_write(&write->request, reinterpret_cast<uv_stream_t*>(&s.tcp->handle), &buf, 1,
                 [](uv_write_t* req, int status) {
                   std::unique_ptr<Abomin::Write> write(static_cast<Abomin::Write*>(req->data));
                   write->session->writing = false;
                   if (status < 0) write->session->failure = uv_strerror(status);
                 });
  if (status < 0) {
    s.writing = false;
    delete write;
    s.failure = uv_strerror(status);
    return -1;
  }
  return static_cast<std::ptrdiff_t>(len);
}

int ab_io_flush(void* io) {
  auto& s = *static_cast<Abomin::Session*>(io);
  return s.closed || !s.failure.empty() ? -1 : s.writing ? -2 : 0;
}

void ab_io_wake(void* io) {
  auto& s = *static_cast<Abomin::Session*>(io);
  s.runtime->inbox()->send({Abomin::Envelope::NetworkReady, 0, {}});
}
}
