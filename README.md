# Abomin

A native JavaScript and TypeScript runtime built around JavaScriptCore, C++26,
libuv, and Rust services. The executable embeds its JavaScript standard library;
Node.js, Bun, Deno, and a TypeScript compiler are not runtime dependencies.

```sh
abomin run examples/hello.ts one two
abomin examples/worker.ts
abomin eval 'console.log(await crypto.subtle.digest("SHA-256", new TextEncoder().encode("hello")))'
abomin examples/fetch.ts https://example.com
```

## Build

Supported build targets are macOS arm64, Linux x64, and Windows x64.
Toolchain versions are recorded in `scripts/toolchains.json` and
`rust-toolchain.toml`: Clang 23.1.3, Rust 1.98.0, and xmake 3.1.1.
Python 3.12 or newer bootstraps the engine SDK. CMake and a native platform SDK
are needed for native dependencies. Windows uses clang-cl and the Visual Studio
2022 x64 build environment; Linux uses LLVM's libc++.

```sh
python3 scripts/setup.py
xmake f -y --toolchain=clang -m release
xmake -y
xmake run abomin examples/hello.ts
```

On Windows, replace `python3` with `python` and use `--toolchain=clang-cl`.
Run `python scripts/install_llvm.py` to install the pinned LLVM release with the
GitHub CLI; add the printed `bin` directory to `PATH` in a local shell. Apply the printed `SDKROOT`
(macOS) or `LD_LIBRARY_PATH` (Linux) value for build tools as well.
With this standalone LLVM SDK, pass its printed `ABOMIN_LLVM_ROOT` directory to
`xmake f --sdk=<directory>` so libc++ and libc++abi are linked from the same SDK.
The manually triggered **Build runtime** GitHub workflow installs these tools,
uses platform/configuration-specific caches, runs the tests, and uploads all
three binary artifacts. Select `debug`, `release`, or `benchmark` when starting it.

The JavaScriptCore SDK is fixed to one release ABI for all three application
configurations. Its generated configuration, headers, static libraries, and ICU
are kept together. Application debug symbols do not enable a different JSC ABI.
Rust is linked through one static library and a C ABI. `Cargo.lock` and xmake's
exact native dependency versions are committed. Cached SDK downloads are
SHA-256 checked; a version/platform/configuration-specific verification stamp
prevents reusing a different engine artifact.

## Runtime model

Each main runtime or Dedicated Module Worker owns one execution thread, one
JavaScriptCore VM, one libuv loop, local resources, and local HTTP futures.
JavaScriptCore is accessed only from C++. Rust never receives JS values.

Native operations have fixed names and typed arguments. JavaScript owns the
Promise table; C++ returns completions by call ID on the owning loop. Each task
is followed by a microtask checkpoint. Sources and Oxc output are process-shared (256 entries / 64 MiB; individual
source files are limited to 16 MiB);
module instances and live bindings belong to their VM. CPU-heavy digest work
uses a shared pool of up to four native threads with a 256-item queue.

File and network buffers are copied before asynchronous work starts. Closing a
file removes its local resource ID while in-flight operations retain its native
handle. Mailboxes carry owned clone packets and no VM pointers. A Worker mailbox
allows at most 1,024 pending messages and 64 MiB of payload. Each clone packet is
limited to 16 MiB. MessagePort queues allow 1,024 pending packets.

HTTP protocol processing stays inside hyper and rustls, using AWS-LC. The
transport implements Rust asynchronous I/O through libuv DNS, TCP reads and
writes. A dedicated Tokio runtime is not started. TLS uses WebPKI trust roots and
verifies both certificates and host names; `ABOMIN_CA_FILE` can add PEM trust roots.
ALPN negotiates HTTP/2 or HTTP/1.1. Response bodies pull native chunks with
backpressure. Cancelling the body or its AbortSignal closes the connection.

## APIs

| Area | Implemented |
|---|---|
| Execution | JavaScript, TypeScript/TSX/JSX transpilation through Oxc; native ESM, live bindings, cycles, dynamic import, top-level await, `import.meta.url` |
| Modules | Explicit `.js`, `.mjs`, `.ts`, `.mts`, `.jsx`, `.tsx` files; relative, absolute and local `file:` URLs |
| Scheduling | Promise, microtasks, timeouts, intervals, monotonic performance clock, uncaught error and unhandled rejection reporting |
| Workers | Dedicated module Workers, error/exit propagation, nested Workers, `terminate()` interrupting running JS, parent/child `postMessage` |
| Messaging | Cyclic graphs, Map, Set, Date, RegExp, Error, Blob/File, BigInt, typed arrays, ArrayBuffer transfer, MessageChannel and MessagePort transfer |
| Web primitives | EventTarget/events, AbortController/Signal, URL/SearchParams (Ada), streaming text encoding/decoding, Base64, Blob, File, FormData |
| Streams | ReadableStream, WritableStream, TransformStream, BYOB readers and standard queuing strategies through Web Streams Polyfill |
| Fetch | HTTP/1.1, HTTP/2 over TLS, `data:` URLs, Request, Response, Headers, redirects, streaming downloads, abort and content decoding |
| Compression | Streaming gzip, deflate, raw deflate (zlib-ng), Brotli |
| Crypto | Secure random values, UUID v4, SHA-1/256/384/512 digest; AWS-LC |
| Host | `Abomin.args`, `version`, `mainModule`, `cwd`, environment reads, asynchronous file handles and streams, whole-file helpers, stat/mkdir/remove |
| Engine | JSC ECMAScript, Intl/ICU, engine-provided WebAssembly |

Add `types/abomin.d.ts` to an editor's TypeScript configuration for the host API.
Web API names are supplied by TypeScript's DOM declarations; the table above
and the limitations below define the implemented runtime surface.

The standard library targets server/CLI Web APIs and WinterTC portability.
This version has not passed full WinterTC/WPT conformance and does not promise
complete browser compatibility. Notable current boundaries:

- TypeScript is transpiled, without type checking; JSX output expects the
  configured JSX runtime in the user program. Node/npm/CommonJS and remote module
  imports are not implemented. File module imports require explicit extensions.
- Fetch uploads are buffered up to 64 MiB. Use response streams for large
  downloads; whole-body/file helpers are limited to 256 MiB. Connections are
  currently per request, without a connection pool. HTTP server, WebSocket,
  cookies, cache storage, and browser CORS enforcement are not implemented.
- `crypto.subtle` currently implements digests. Key import/export, signing,
  encryption, and key derivation are not implemented.
- Multipart FormData can be sent; `Body.formData()` currently parses URL-encoded
  bodies only. SharedArrayBuffer and shared-memory cloning are not supported.
- A Worker or MessagePort must be explicitly closed when its work is complete.
  File/loop shutdown drains native work; the runtime is not a security sandbox.
- `Abomin.exit()` requests shutdown after the current JS task; it is not an OS
  process-exit primitive. This is an initial runtime release, not a production
  compatibility certification.

## Validation

```sh
cargo test --locked
python3 tests/run.py --binary build/macosx/arm64/release/abomin
```

The integration runner launches the actual executable. It covers TS modules,
live bindings, dynamic import, timers, Unicode/NUL data, incremental decoding,
compression, structured cloning/transfer, asynchronous files, Worker messaging,
MessagePort handoff, interrupting infinite Worker execution, CLI errors,
unresolved top-level await, HTTP redirects, streaming and cancellation,
parallel fetch, TLS rejection, additional trust roots and HTTP/2 ALPN.
The HTTP/2 fixture uses Node.js and OpenSSL as test tools; they are not needed
by the resulting binary. The Rust service tests cover transpilation errors and
known digest results.
