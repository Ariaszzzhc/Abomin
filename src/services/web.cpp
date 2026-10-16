#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>

#include <array>

#include "core/runtime.hpp"
#include "core/value.hpp"
#include "services/ffi.hpp"

namespace Abomin {
  namespace {
    struct Decoder {
      void* value{};
      bool finished{};

      ~Decoder() {
        ab_decoder_free(value);
      }
    };

    struct Compression {
      bool decode{}, brotli{}, finished{};
      z_stream z{};
      BrotliEncoderState* encoder{};
      BrotliDecoderState* decoder{};

      ~Compression() {
        if (brotli) {
          BrotliEncoderDestroyInstance(encoder);
          BrotliDecoderDestroyInstance(decoder);
        } else if (decode)
          inflateEnd(&z);
        else
          deflateEnd(&z);
      }
    };

    JSClassRef decoderClass() {
      static auto cls = [] {
        auto d = kJSClassDefinitionEmpty;
        d.className = "DecoderState";
        d.finalize = [](JSObjectRef o) { delete static_cast<Decoder*>(JSObjectGetPrivate(o)); };
        return JSClassCreate(&d);
      }();
      return cls;
    }

    JSClassRef compressionClass() {
      static auto cls = [] {
        auto d = kJSClassDefinitionEmpty;
        d.className = "CompressionState";
        d.finalize = [](JSObjectRef o) { delete static_cast<Compression*>(JSObjectGetPrivate(o)); };
        return JSClassCreate(&d);
      }();
      return cls;
    }

    JSValueRef take(JSContextRef ctx, AbBuffer result) {
      std::string text(reinterpret_cast<char*>(result.data), result.len);
      bool failed = result.error;
      ab_buffer_free(result);
      if (failed) throw std::runtime_error(text);
      return string(ctx, text);
    }

    JSValueRef encodingLabel(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t n,
                             const JSValueRef a[], JSValueRef* e) {
      try {
        if (!n) throw std::runtime_error("Missing encoding");
        auto label = text(ctx, a[0]);
        return take(ctx, ab_encoding_label(reinterpret_cast<const std::uint8_t*>(label.data()),
                                           label.size()));
      } catch (const std::exception& err) {
        *e = error(ctx, err.what());
        return JSValueMakeUndefined(ctx);
      }
    }

    JSValueRef decoder(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t n, const JSValueRef a[],
                       JSValueRef* e) {
      try {
        if (n < 2) throw std::runtime_error("Missing encoding options");
        auto label = text(ctx, a[0]);
        auto value = ab_decoder_new(reinterpret_cast<const std::uint8_t*>(label.data()),
                                    label.size(), JSValueToBoolean(ctx, a[1]));
        if (!value) throw std::runtime_error("Invalid encoding");
        return JSObjectMake(ctx, decoderClass(), new Decoder{value});
      } catch (const std::exception& err) {
        *e = error(ctx, err.what());
        return JSValueMakeUndefined(ctx);
      }
    }

    JSValueRef decoderPush(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t n,
                           const JSValueRef a[], JSValueRef* e) {
      try {
        if (n < 4 || !JSValueIsObjectOfClass(ctx, a[0], decoderClass()))
          throw std::runtime_error("Invalid decoder");
        auto* state
            = static_cast<Decoder*>(JSObjectGetPrivate(JSValueToObject(ctx, a[0], nullptr)));
        if (state->finished) throw std::runtime_error("Decoder is finished");
        auto input = bytes(ctx, a[1]);
        bool last = JSValueToBoolean(ctx, a[3]);
        state->finished = last;
        return take(ctx, ab_decoder_push(state->value, input.data(), input.size(),
                                         JSValueToBoolean(ctx, a[2]), last));
      } catch (const std::exception& err) {
        *e = error(ctx, err.what());
        return JSValueMakeUndefined(ctx);
      }
    }

    JSValueRef compression(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t n,
                           const JSValueRef a[], JSValueRef* e) {
      try {
        if (n < 2) throw std::runtime_error("Missing compression options");
        auto format = text(ctx, a[0]);
        auto state = std::make_unique<Compression>();
        state->decode = JSValueToBoolean(ctx, a[1]);
        state->brotli = format == "brotli";
        if (state->brotli) {
          if (state->decode)
            state->decoder = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
          else
            state->encoder = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
          if (!state->decoder && !state->encoder)
            throw std::runtime_error("Cannot allocate Brotli state");
        } else {
          auto bits = format == "gzip"          ? 31
                      : format == "deflate-raw" ? -15
                      : format == "deflate"     ? 15
                                                : 0;
          if (!bits) throw std::runtime_error("Invalid compression format");
          auto result = state->decode ? inflateInit2(&state->z, bits)
                                      : deflateInit2(&state->z, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                                     bits, 8, Z_DEFAULT_STRATEGY);
          if (result != Z_OK) throw std::runtime_error("Cannot initialize compression");
        }
        return JSObjectMake(ctx, compressionClass(), state.release());
      } catch (const std::exception& err) {
        *e = error(ctx, err.what());
        return JSValueMakeUndefined(ctx);
      }
    }

    JSValueRef compressionPush(JSContextRef ctx, JSObjectRef, JSObjectRef, size_t n,
                               const JSValueRef a[], JSValueRef* e) {
      try {
        if (n < 3 || !JSValueIsObjectOfClass(ctx, a[0], compressionClass()))
          throw std::runtime_error("Invalid compression state");
        auto* s
            = static_cast<Compression*>(JSObjectGetPrivate(JSValueToObject(ctx, a[0], nullptr)));
        if (s->finished) {
          if (JSValueToBoolean(ctx, a[2]) && bytes(ctx, a[1]).empty()) return buffer(ctx, {});
          throw std::runtime_error("Compression stream already finished");
        }
        auto input = bytes(ctx, a[1]);
        const bool last = JSValueToBoolean(ctx, a[2]);
        std::vector<std::uint8_t> output;
        std::array<std::uint8_t, 16384> chunk;
        if (s->brotli) {
          const auto* in = input.data();
          size_t remaining = input.size();
          while (true) {
            auto* out = chunk.data();
            size_t available = chunk.size();
            if (s->decode) {
              auto result = BrotliDecoderDecompressStream(s->decoder, &remaining, &in, &available,
                                                          &out, nullptr);
              output.insert(output.end(), chunk.begin(), chunk.end() - available);
              if (result == BROTLI_DECODER_RESULT_ERROR
                  || (last && result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT))
                throw std::runtime_error("Invalid Brotli stream");
              if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                s->finished = true;
                if (remaining) throw std::runtime_error("Trailing compressed data");
                break;
              }
              if (!remaining && result != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) break;
            } else {
              if (!BrotliEncoderCompressStream(
                      s->encoder, last ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS,
                      &remaining, &in, &available, &out, nullptr))
                throw std::runtime_error("Brotli compression failed");
              output.insert(output.end(), chunk.begin(), chunk.end() - available);
              if (!remaining && !BrotliEncoderHasMoreOutput(s->encoder)) {
                if (last) s->finished = true;
                break;
              }
            }
            if (output.size() > 64 * 1024 * 1024)
              throw std::runtime_error("Compression output exceeds 64 MiB per chunk");
          }
        } else {
          if (input.size() > UINT_MAX) throw std::runtime_error("Compression input too large");
          s->z.next_in = input.data();
          s->z.avail_in = static_cast<uInt>(input.size());
          while (true) {
            s->z.next_out = chunk.data();
            s->z.avail_out = chunk.size();
            auto result = s->decode ? inflate(&s->z, last ? Z_FINISH : Z_NO_FLUSH)
                                    : deflate(&s->z, last ? Z_FINISH : Z_NO_FLUSH);
            output.insert(output.end(), chunk.begin(), chunk.end() - s->z.avail_out);
            if (result == Z_STREAM_END) {
              s->finished = true;
              if (s->z.avail_in) throw std::runtime_error("Trailing compressed data");
              break;
            }
            if (result != Z_OK && result != Z_BUF_ERROR)
              throw std::runtime_error("Invalid compressed stream");
            if (!s->z.avail_in && s->z.avail_out) {
              if (last) throw std::runtime_error("Truncated compressed stream");
              break;
            }
            if (output.size() > 64 * 1024 * 1024)
              throw std::runtime_error("Compression output exceeds 64 MiB per chunk");
          }
        }
        return buffer(ctx, output);
      } catch (const std::exception& err) {
        *e = error(ctx, err.what());
        return JSValueMakeUndefined(ctx);
      }
    }
  }  // namespace

  void installWebOps(Runtime& r, JSObjectRef native) {
    for (auto [name, fn] :
         std::initializer_list<std::pair<const char*, JSObjectCallAsFunctionCallback>>{
             {"encodingLabel", encodingLabel},
             {"decoder", decoder},
             {"decoderPush", decoderPush},
             {"compression", compression},
             {"compressionPush", compressionPush}})
      property(r.context(), native, name,
               JSObjectMakeFunctionWithCallback(r.context(), String(name), fn));
  }
}  // namespace Abomin
