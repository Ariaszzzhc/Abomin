#pragma once
#include <string>
#include <string_view>

namespace Abomin {
  /** Shared immutable sources; each VM instantiates its own module graph.
   * 共享不可变源码；每个 VM 独立实例化模块图。 */
  class ModuleLoader {
  public:
    static std::string resolve(std::string_view specifier, std::string_view referrer = {},
                               bool requireFile = true);
    static std::string source(const std::string& url);
    static std::string path(const std::string& url);
  };
}  // namespace Abomin
