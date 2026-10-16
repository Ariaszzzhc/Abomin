#pragma once
#include <cstddef>
#include <cstdint>

/** Stable native ABI; buffers never borrow VM memory across async calls.
 * 稳定原生 ABI；异步调用不借用 VM 内存。 */
extern "C" {
struct AbBuffer {
  std::uint8_t* data;
  std::size_t len;
  std::size_t capacity;
  std::int32_t error;
};

void ab_buffer_free(AbBuffer);
AbBuffer ab_transpile(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t);
AbBuffer ab_decode(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t, bool);
AbBuffer ab_base64(const std::uint8_t*, std::size_t, bool);
bool ab_random(std::uint8_t*, std::size_t);
AbBuffer ab_digest(std::uint32_t, const std::uint8_t*, std::size_t);
}
extern "C" {
AbBuffer ab_encoding_label(const std::uint8_t*, std::size_t);
void* ab_decoder_new(const std::uint8_t*, std::size_t, bool);
void ab_decoder_free(void*);
AbBuffer ab_decoder_push(void*, const std::uint8_t*, std::size_t, bool, bool);
}
extern "C" {
struct AbHeader {
  const std::uint8_t* name;
  std::size_t name_len;
  const std::uint8_t* value;
  std::size_t value_len;
};

struct AbView {
  const std::uint8_t* data;
  std::size_t len;
};

void* ab_http_new(void*, const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t,
                  const AbHeader*, std::size_t, const std::uint8_t*, std::size_t);
int ab_http_poll(void*);
std::uint16_t ab_http_status(const void*);
bool ab_http_header(const void*, std::size_t, AbView*, AbView*);
AbBuffer ab_http_error(const void*);
AbBuffer ab_http_read(void*);
void ab_http_free(void*);
}
