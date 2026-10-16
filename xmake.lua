set_project("abomin")
set_version("0.1.0")
set_xmakever("3.0.0")
set_languages("c++26")
local host = is_host("macosx") and "macos" or os.host()
local arch = os.arch() == "arm64" and "arm64" or "amd64"
local jsc_default = ".cache/jsc/34c01d13391e00c06862a3d2c5b7fff350ac87e0/" .. host .. "-" .. arch .. "/release/bun-webkit"
add_rules("mode.debug", "mode.release")
if is_plat("linux") then
  set_runtimes("c++_static")
  add_requireconfs("*", {configs = {runtimes = "c++_static"}})
elseif is_plat("windows") then
  set_runtimes("MT")
  add_requireconfs("*", {configs = {runtimes = "MT"}})
end

option("jsc")
  set_default(jsc_default)
  set_description("Pinned JavaScriptCore SDK directory")
option_end()

add_requires("libuv 1.52.1", "ada 3.4.4", "simdutf 8.2.0", "mimalloc 3.2.8",
             "brotli 1.2.0", {configs = {shared = false}})

add_requires("zlib-ng 2.3.3", {configs = {shared = false, zlib_compat = true}})

target("rust_services")
  set_kind("phony")
  on_build(function (target)
    local mode = is_mode("debug") and "debug" or "release"
    local args = {"build", "--locked"}
    if mode == "release" then table.insert(args, "--release") end
    os.vrunv("cargo", args)
  end)
target_end()

target("abomin")
  set_kind("binary")
  add_deps("rust_services")
  add_files("src/**.cpp")
  add_includedirs("src", "build/generated")
  add_packages("libuv", "ada", "simdutf", "mimalloc", "zlib-ng", "brotli")
  add_defines("BUILDING_JSCONLY__=1", "BUILDING_WITH_CMAKE=1", "JSC_OBJC_API_ENABLED=0",
              "STATICALLY_LINKED_WITH_JavaScriptCore=1", "NDEBUG", "RELEASE_WITHOUT_OPTIMIZATIONS=1")
  on_load(function (target)
    local sdk = path.absolute(get_config("jsc"))
    target:add("includedirs", sdk .. "/include")
    target:add("linkdirs", sdk .. "/lib", "target/" .. (is_mode("debug") and "debug" or "release"))
    target:add("linkdeps", path.join("target", is_mode("debug") and "debug" or "release", is_plat("windows") and "abomin_services.lib" or "libabomin_services.a"))
  end)
  before_build(function (target)
    os.mkdir("build/generated")
    local js = "(function(){ const exports = {}; const module = {exports};\n" .. io.readfile("runtime/vendor/streams.js") .. "\nObject.assign(globalThis, exports); })();\n(function(){\n"
    for _, name in ipairs({"base", "web", "clone", "io", "fetch", "bootstrap"}) do
      js = js .. io.readfile("runtime/" .. name .. ".js") .. "\n"
    end
    js = js .. "})();\n"
    local content = "#pragma once\n#include <string_view>\nnamespace Abomin { inline constexpr std::string_view bootstrap = R\"ABOMIN_JS(" .. js .. ")ABOMIN_JS\"; }\n"
    if not os.isfile("build/generated/builtins.hpp") or io.readfile("build/generated/builtins.hpp") ~= content then
      io.writefile("build/generated/builtins.hpp", content)
    end
  end)
  add_links("abomin_services", "JavaScriptCore", "WTF", "bmalloc")
  if is_plat("macosx") then
    add_syslinks("icucore", "resolv", "iconv")
    add_frameworks("Foundation", "Security")
  elseif is_plat("linux") then
    add_cxxflags("-stdlib=libc++", {force = true})
    add_ldflags("-stdlib=libc++", "-no-pie", {force = true})
    add_links("icui18n", "icuuc", "icudata")
    add_syslinks("pthread", "dl", "m", "atomic", "unwind")
  elseif is_plat("windows") then
    set_runtimes("MT")
    add_defines("NOMINMAX", "U_STATIC_IMPLEMENTATION")
    add_cxxflags("/GR-", "/Zc:__cplusplus", {force = true})
    add_links("sicuin", "sicuuc", "sicudt")
    add_syslinks("ws2_32", "userenv", "bcrypt", "ntdll", "dbghelp", "psapi", "winmm", "advapi32", "crypt32", "iphlpapi", "shlwapi", "shell32", "ole32")
  end
  if is_mode("benchmark") then
    set_optimize("fastest")
    set_symbols("debug")
  end
  if not is_plat("windows") then add_cxxflags("-fno-rtti", {force = true}) end
  set_warnings("all")
target_end()
