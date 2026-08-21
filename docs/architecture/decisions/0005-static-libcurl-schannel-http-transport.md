# ADR 0005: Use static libcurl with Schannel for HTTP transport

- Status: Accepted; native-Windows runtime validation remains in the release matrix
- Date: 2026-08-20

## Context

dps.report, GW2Wingman, DonBot, and Twitch need one provider-independent HTTP boundary. It must stream
large bodies from a pull source, support arbitrary methods including `PATCH`, cancel promptly during
blocked transfers, enforce response limits while receiving, verify TLS on Windows and Wine, and ship
without a companion HTTP or TLS DLL.

The spike compared two Windows implementations:

- **Synchronous WinHTTP:** built into Windows and small, but Microsoft explicitly says not to close a
  synchronous request handle while an API call is pending. This prevents another owner thread from
  safely using handle closure as prompt cancellation. The asynchronous API can cancel by closing a
  handle, but requires callback-status accounting and strict lifetime synchronization through the
  final handle-closing notification.
- **Static libcurl:** adds bundled code, but its read callback directly models a bounded pull source,
  its progress callback can abort a blocked transfer, redirects are disabled by default, and its easy
  handle has an unambiguous per-request owner. Schannel supplies Windows TLS and the native trust
  store, so OpenSSL and a certificate bundle are unnecessary.

The relevant primary documentation is Microsoft's
[`WinHttpCloseHandle`](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle)
and [WinHTTP concurrency guidance](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp),
plus libcurl's [`CURLOPT_READFUNCTION`](https://curl.se/libcurl/c/CURLOPT_READFUNCTION.html),
[`CURLOPT_XFERINFOFUNCTION`](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html), and
[`CURLOPT_FOLLOWLOCATION`](https://curl.se/libcurl/c/CURLOPT_FOLLOWLOCATION.html) documentation.

## Decision

Use libcurl 8.21.0 from the official release archive:

- URL: `https://curl.se/download/curl-8.21.0.tar.xz`
- SHA-256: `aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6`
- linkage: static, with static compiler runtimes for MinGW verification builds;
- protocols: HTTP and HTTPS only;
- TLS: Schannel with native certificate authorities, peer verification, hostname verification, and a
  TLS 1.2 minimum;
- omitted features: cookies, netrc, HSTS, Alt-Svc, DoH, MIME, built-in HTTP authentication, PSL,
  Brotli, zlib, zstd, HTTP/2, and non-HTTP protocols; and
- disabled runtime behavior: automatic redirects, unrestricted credential forwarding, verbose wire
  traces, and HTTP-error-to-transport-error conversion.

Production requests accept HTTPS only. An explicit construction policy permits HTTP solely for
loopback fake-server tests and bypasses proxies in that test mode. The adapter creates one easy handle
per synchronous `execute` call. Request bodies are pulled into libcurl-owned bounded spans, and all
read, header, body, and progress callbacks contain C++ exceptions.

Global libcurl initialization occurs when the application constructs the first adapter, never in
`DllMain` or static initialization. A locked user count prevents cleanup from racing construction;
the last adapter performs cleanup after provider workers have stopped and joined.

## Verification evidence

The pinned configuration cross-compiles with MinGW warnings-as-errors and reports only `http` and
`https` protocols with Schannel enabled. The default Windows suite passes 1,235 deterministic checks
under Wine, including:

- streamed POST request framing and exact body bytes;
- duplicate headers and sensitive response-header classification;
- returning a 302 without following its `Location`;
- incremental response header/body limit enforcement;
- early EOF and invalid source byte-count rejection;
- exception containment and redaction at the upload-source callback boundary;
- coexistence and ordered destruction of multiple adapter instances; and
- cancellation of a declared 512 MiB streaming upload in under three seconds without allocating the
  declared body.

An opt-in live probe using the production policy completed `https://example.com/` under Wine with
status 200 through `libcurl/8.21.0 Schannel`. A stripped MinGW probe containing the actual adapter,
static libcurl, and static compiler runtimes was approximately 1.6 MiB and imported only Windows
system DLLs. The default test suite never contacts a public service.

These results validate the maintained Wine path and deployment shape. Native Windows still needs the
same deterministic suite and TLS probe in CI or the release matrix before a release claim is made.

## Consequences

- Provider clients receive a small typed C++ port and never include curl types.
- Cancellation does not depend on closing a synchronous Windows handle from another thread.
- The release remains a single addon artifact with no libcurl, OpenSSL, or MinGW companion DLL.
- libcurl increases binary size and becomes a security-sensitive pinned dependency whose updates need
  hash, license, Windows, Wine, TLS, cancellation, and deployment verification.
- Redirect validation and retry/status classification remain explicit provider-layer behavior.
- If native-Windows testing uncovers a Schannel or lifecycle defect that cannot be corrected cleanly,
  this decision must be revisited rather than weakening certificate or hostname verification.
