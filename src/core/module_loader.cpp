#include "core/module_loader.hpp"

#include <ada.h>

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
    };

    std::mutex cacheMutex;
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
      auto p = std::filesystem::absolute(std::filesystem::u8path(specifier))
                   .lexically_normal()
                   .generic_string();
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
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(filename)))
      throw std::runtime_error("Module not found: " + result);
    return result;
  }

  std::string ModuleLoader::source(const std::string& url) {
    auto filename = path(url);
    auto file = std::filesystem::u8path(filename);
    auto modified = std::filesystem::last_write_time(file);
    auto size = std::filesystem::file_size(file);
    std::lock_guard lock(cacheMutex);
    if (auto found = cache.find(url);
        found != cache.end() && found->second.modified == modified && found->second.size == size)
      return found->second.code;
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
    cache.insert_or_assign(url, CachedSource{modified, size, code});
    return code;
  }
}  // namespace Abomin
