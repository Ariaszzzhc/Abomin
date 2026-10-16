# Third-party components

The build downloads fixed upstream artifacts and uses their respective licenses.
Upstream source and license texts are available at the exact versions below.

| Component | Version | License / source |
|---|---|---|
| JavaScriptCore, WTF, bmalloc | 34c01d13391e00c06862a3d2c5b7fff350ac87e0 | [WebKit licenses and source](https://github.com/oven-sh/WebKit/tree/autobuild-34c01d13391e00c06862a3d2c5b7fff350ac87e0) (BSD and LGPL; individual files apply) |
| ICU | Bundled with JSC on Linux/Windows; system ICU on macOS | [ICU license](https://github.com/unicode-org/icu/blob/main/LICENSE) |
| libuv | 1.52.1 | [MIT](https://github.com/libuv/libuv/blob/v1.52.1/LICENSE) |
| Ada | 3.4.4 | [Apache-2.0 / MIT](https://github.com/ada-url/ada/tree/v3.4.4) |
| simdutf | 8.2.0 | [Apache-2.0 / MIT](https://github.com/simdutf/simdutf/tree/v8.2.0) |
| mimalloc | 3.2.8 | [MIT](https://github.com/microsoft/mimalloc/blob/v3.2.8/LICENSE) |
| zlib-ng | 2.3.3 | [zlib](https://github.com/zlib-ng/zlib-ng/blob/2.3.3/LICENSE.md) |
| Google Brotli | 1.2.0 | [MIT](https://github.com/google/brotli/blob/v1.2.0/LICENSE) |
| Oxc | 0.154.0 | [MIT](https://github.com/oxc-project/oxc/tree/oxc_v0.154.0) |
| encoding_rs | 0.8.35 | [Apache-2.0 / MIT / BSD-3-Clause](https://github.com/hsivonen/encoding_rs) |
| hyper | 1.7.0 | [MIT](https://github.com/hyperium/hyper/tree/v1.7.0) |
| rustls | 0.23.32 | [Apache-2.0 / ISC / MIT](https://github.com/rustls/rustls/tree/v/0.23.32) |
| AWS-LC Rust | 1.14.1 | [Apache-2.0 / ISC](https://github.com/aws/aws-lc-rs/tree/v1.14.1) |
| Web Streams Polyfill | 4.2.0 | [MIT](runtime/vendor/streams.LICENSE) (vendored with its copyright notice) |

Cargo.lock records exact Rust transitive dependencies and registry checksums.
JSC archives are verified against committed SHA-256 hashes by the setup script.
The vendored Web Streams archive has npm integrity:
`sha512-0rYDzGOh9EZpig92umN5g5D/9A1Kff7k0/mzPSSCY8jEQeYkgRMoY7LhbXtUCWzLCMX0TUE9aoHkjFNB7D9pfA==`.
