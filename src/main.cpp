#include <iostream>
#include <string_view>

#include "core/runtime.hpp"

int main(int argc, char** argv) {
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
