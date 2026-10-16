# ADR 0022: Own native HTTP execution in Rust

- Status: Accepted, Wine and hosted release gates remain outstanding
- Date: 2026-10-16

## Decision

The native addon crate implements the application HttpClient port with the static libcurl 8.21.0
Schannel backend selected in ADR 0005. The portable crate does not depend on C ABI types. Cargo
invokes a small CMake project and builds the pinned source itself. C++ and Rust consume one shared
dependency configuration, preserving the existing archive SHA-256 and disabled features.

The native build requires CMake 3.30. Its newer FetchContent behavior avoids a nested MSBuild
population project. Compact source and build directories keep Cargo's deep paths within MSBuild's
path limits. The generated library manifest identifies the actual static artifact for Cargo.
Rust Debug uses native RelWithDebInfo and Rust Release uses native Release. Both use the release
static CRT required by Rust's crt-static target configuration. A local archive override retains the
same mandatory SHA-256 check. No prebuilt developer library or toolchain file is shipped.

A locked user count owns global initialization and cleanup outside DllMain. Every synchronous
request borrows its client, owns an easy handle, and drops that handle before callback state or its
header list. Workers must still stop and join before runtime teardown. The pinned backend's default
multi cleanup joins its resolver queue. Do not enable its quick-exit option, which can detach resolver
threads and violate hot-unload ownership.

Each request uses fresh state and explicit method, protocol, timeout, response-size, TLS 1.2 minimum,
peer verification, and hostname verification settings. Redirects, unrestricted authentication,
verbose tracing, and HTTP-error-to-transport-error conversion are disabled. Plain HTTP and proxy
bypass are available only through the explicit loopback test policy.

Private bindings preserve the pinned Windows x64 C ABI, including 32-bit C long option arguments
and 64-bit curl_off_t sizes. All callback functions contain Rust panics and retain the first typed
error. Sources pass through the exact-length guard, while response callbacks use ResponseCollector.
Cancellation is checked before connection, in every callback, and before returning a response.
Backend messages are never included in diagnostics. Only safe categories and numeric codes escape.

Header assembly reserves capacity before copying values and wipes the temporary line. List ownership
walks and wipes every libcurl-owned header copy before freeing the list. Explicit empty header values
use libcurl's semicolon syntax. The separate Expect suppression uses a colon. A wire-level regression
test distinguishes these two behaviors. URL staging also wipes its owned temporary bytes, without
claiming to wipe copies inside dependencies or operating-system buffers.

## Verification and remaining work

Native loopback tests exercise all supported methods, binary and multipart uploads, explicit empty
headers, duplicates, sensitive headers, interim responses, chunked trailers, redirect non-following,
HTTP error statuses, cumulative response caps, timeouts, cancellation, callback source failures,
and overlapping runtime owners. A declared 512 MiB source cancels without allocating that size.
Runtime inspection confirms the pinned version, Schannel, HTTP/HTTPS-only protocols, and omitted
compression, HTTP/2, Alt-Svc, and HSTS features. An HTTPS request to a plaintext loopback server
fails with a TLS error. An ignored public-service probe is available for explicit Schannel checks.

The addon lifecycle shell does not yet construct this client. Provider clients, authentication
workflows, application composition, active-request unload tests, Wine execution, packaged Rust
deployment checks, and hosted native release gates remain required.

References: [libcurl option argument types](https://curl.se/libcurl/c/curl_easy_setopt.html),
[global initialization](https://curl.se/libcurl/c/curl_global_init.html),
[read callbacks](https://curl.se/libcurl/c/CURLOPT_READFUNCTION.html),
[progress callbacks](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html).
