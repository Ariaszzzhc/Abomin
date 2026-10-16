#include "core/module_loader.hpp"

#include <ada.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "services/ffi.hpp"

namespace Abomin {
  namespace {
    struct CachedSource {
      std::filesystem::file_time_type modified;
      std::uintmax_t size;
      std::string code;
      std::uint64_t used;
    };

    std::filesystem::path utf8Path(std::string_view value) {
      return std::filesystem::path(
          std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    }

    std::mutex cacheMutex;
    std::uint64_t cacheClock{};
    std::size_t cachedBytes{};
    std::unordered_map<std::string, CachedSource> cache;
  }  // namespace

  std::string ModuleLoader::path(const std::string& url) {
    auto parsed = ada::parse<ada::url_aggregator>(url);
    if (!parsed || parsed->get_protocol() != "file:" || !parsed->get_hostname().empty())
      throw std::runtime_error("Only local file modules are supported: " + url);
    auto result
        = ada::unicode::percent_decode(parsed->get_pathname(), parsed->get_pathname().find('%'));
#ifdef _WIN32
    if (result.size() > 2 && result[0] == '/' && result[2] == ':') result.erase(0, 1);
#endif
    if (result.find('\0') != std::string::npos) throw std::runtime_error("Invalid module path");
    return result;
  }

  std::string ModuleLoader::resolve(std::string_view specifier, std::string_view referrer) {
    std::string result;
    if (specifier.starts_with("file:"))
      result = specifier;
    else if (!referrer.empty()) {
      if (!specifier.starts_with("./") && !specifier.starts_with("../")
          && !specifier.starts_with('/'))
        throw std::runtime_error("Bare and remote module specifiers are unsupported: "
                                 + std::string(specifier));
      auto base = ada::parse<ada::url_aggregator>(referrer);
      auto url = ada::parse<ada::url_aggregator>(specifier, base ? &*base : nullptr);
      if (!url) throw std::runtime_error("Invalid module URL");
      result = url->get_href();
    } else {
      auto normalized
          = std::filesystem::absolute(utf8Path(specifier)).lexically_normal().generic_u8string();
      std::string p(reinterpret_cast<const char*>(normalized.data()), normalized.size());
#ifdef _WIN32
      p = "/" + p;
#endif
      result = "file://";
      for (unsigned char c : p) {
        if (c == '%' || c == '#' || c == '?' || c == ' ' || c >= 128) {
          constexpr char hex[] = "0123456789ABCDEF";
          result += '%';
          result += hex[c >> 4];
          result += hex[c & 15];
        } else
          result += c;
      }
    }
    auto filename = path(result);
    if (!std::filesystem::is_regular_file(utf8Path(filename)))
      throw std::runtime_error("Module not found: " + result);
    return result;
  }

  std::string ModuleLoader::source(const std::string& url) {
    auto filename = path(url);
    auto file = utf8Path(filename);
    auto modified = std::filesystem::last_write_time(file);
    auto size = std::filesystem::file_size(file);
    if (size > 16 * 1024 * 1024) throw std::runtime_error("Module exceeds 16 MiB: " + url);
    {
      std::lock_guard lock(cacheMutex);
      if (auto found = cache.find(url); found != cache.end() && found->second.modified == modified
                                        && found->second.size == size) {
        found->second.used = ++cacheClock;
        return found->second.code;
      }
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read module: " + url);
    std::ostringstream contents;
    contents << input.rdbuf();
    auto raw = contents.str();
    auto ext = file.extension().string();
    if (ext != ".js" && ext != ".mjs" && ext != ".ts" && ext != ".mts" && ext != ".tsx"
        && ext != ".jsx")
      throw std::runtime_error("Unsupported module extension: " + ext);
    auto output
        = ab_transpile(reinterpret_cast<const std::uint8_t*>(filename.data()), filename.size(),
                       reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
    std::string code(reinterpret_cast<char*>(output.data), output.len);
    bool failed = output.error;
    ab_buffer_free(output);
    if (failed) throw std::runtime_error(code);
    {
      std::lock_guard lock(cacheMutex);
      if (auto found = cache.find(url); found != cache.end()) {
        cachedBytes -= found->second.code.size();
        cache.erase(found);
      }
      constexpr std::size_t budget = 64 * 1024 * 1024;
      if (code.size() <= budget) {
        while (!cache.empty() && (cache.size() >= 256 || cachedBytes + code.size() > budget)) {
          auto oldest = std::min_element(
              cache.begin(), cache.end(),
              [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
          cachedBytes -= oldest->second.code.size();
          cache.erase(oldest);
        }
        cachedBytes += code.size();
        cache.emplace(url, CachedSource{modified, size, code, ++cacheClock});
      }
    }
    return code;
  }
}  // namespace Abomin
