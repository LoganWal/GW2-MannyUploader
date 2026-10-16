# ADR 0021: Stream Rust multipart bodies and bound responses

- Status: Accepted
- Date: 2026-10-16

## Decision

The HTTP adapter composes multipart bodies from bounded framing and owned single-use sources.
Each source has an exact-length guard. Framing counts toward the 512 MiB request limit, and no
constructor reads or buffers the complete upload. Empty fields are valid. A source failure or
cancellation permanently fails the form and drops remaining sources, including wiping secret fields.

Preserve the existing 16-part limit, 70-byte boundary limit, restricted field and filename syntax,
and exact CRLF framing. Content types additionally have a 255-byte cap before framing allocation.
The application owns a RandomSource port. The native implementation uses BCryptGenRandom with the
system-preferred RNG. Boundaries contain 16 random bytes encoded as hexadecimal. Failure or panic
in the random source aborts construction without a constant or timestamp fallback. Explicit
boundaries support deterministic tests.

ResponseCollector accepts complete backend header lines and arbitrary body chunks. All status,
interim, blank, and trailer lines consume the same cumulative header budget. Final response headers
replace interim headers. Duplicate headers and trailers retain their order. Header names are HTTP
tokens, values reject controls, and non-UTF-8 value bytes are preserved in wiping storage. Known
sensitive names are annotated automatically.

The collector requires completed final headers and a matching backend status before releasing a
response. HTTP 3xx, 4xx, and 5xx remain completed responses. Malformed headers, unsupported protocol
switching, incomplete final responses, cancellation, and exceeded limits return typed safe errors.
Failures are terminal and clear partial header and body values. Response body capacity is reserved
once within the configured limit before the first bytes arrive, avoiding reallocations containing
authentication response data. Completing collection transfers that allocation into SecretValue.

## Verification and remaining work

Tests cover byte-exact multipart framing across pull sizes, binary and empty fields, metadata
injection, framing-inclusive size bounds without allocating large sources, random-source failure,
source failures and panics, terminal cancellation, and actual Windows random boundary generation.
Response tests cover interim headers, duplicate headers, trailers, binary values and bodies,
redaction, exact cumulative limits, malformed messages, status mismatch, and cancellation.

Native static libcurl/Schannel integration and loopback transport tests remain required. The
collector follows libcurl's complete-header-line callback contract and does not parse raw network
chunks. No live service is contacted by these tests. Linux/Wine and hosted release gates remain
separate from native Windows unit and lifecycle checks.

References: [libcurl header callback](https://curl.se/libcurl/c/CURLOPT_HEADERFUNCTION.html),
[Windows system RNG](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgenrandom).
