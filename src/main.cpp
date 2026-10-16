#include <iostream>
#include <string_view>

#include "core/runtime.hpp"

static int runMain(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
    std::cout << "Abomin 0.1.0 — JavaScript / TypeScript runtime\n\n"
                 "Usage: abomin run <file.js|file.ts> [args...]\n"
                 "       abomin <file.js|file.ts> [args...]\n"
                 "       abomin eval <code>\n"
                 "       abomin --version\n";
    return argc < 2 ? 2 : 0;
  }
  if (std::string_view(argv[1]) == "--version" || std::string_view(argv[1]) == "-V") {
    std::cout << "abomin 0.1.0\n";
    return 0;
  }
  bool eval = std::string_view(argv[1]) == "eval";
  int index = eval || std::string_view(argv[1]) == "run" ? 2 : 1;
  if (index >= argc) {
    std::cerr << "Missing entry point\n";
    return 2;
  }
  std::vector<std::string> args;
  for (int i = index + 1; i < argc; ++i) args.emplace_back(argv[i]);
  try {
    Abomin::Runtime runtime(std::move(args));
    return runtime.run(argv[index], eval);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

#ifdef _WIN32
#  include <windows.h>

/** Preserve Unicode command-line paths on Windows. Windows 命令行路径保留 Unicode。 */
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> storage;
  storage.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    if (!size) return 2;
    std::string value(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), size, nullptr, nullptr);
    value.pop_back();
    storage.push_back(std::move(value));
  }
  std::vector<char*> arguments;
  for (auto& value : storage) arguments.push_back(value.data());
  return runMain(argc, arguments.data());
}
#else
int main(int argc, char** argv) {
  return runMain(argc, argv);
}
#endif
