# ADR 0020: Bound Rust HTTP requests and streaming bodies

- Status: Accepted
- Date: 2026-10-16

## Decision

The application owns the synchronous HttpClient and HttpBodySource ports and all request, response,
timeout, limit, and error types. Provider workers may execute requests. Render callbacks may not.
Native HTTP library types remain outside the portable crate and domain.

Preserve the frozen HTTP hard limits, default timeouts, methods, adapter-owned framing headers, and
explicit loopback-only plaintext test policy. Validation checks header count, token syntax, ASCII
values, sensitive annotations, total encoded header bytes, declared body length, timeouts, and
response limits before any source is read. GET cannot have a body. Zero-length whole-request bodies
must be represented by absence, while empty sources remain useful as multipart fields.

Validate URL authority syntax for HTTPS as well as loopback HTTP. Reject user information, fragments,
backslashes, quotes, malformed ports, malformed IPv6, encoded hostnames, and invalid DNS labels before
the backend parses them. This deliberately tightens the C++ port's HTTPS validation to avoid parser
ambiguity. Hostnames are ASCII and may use punycode. Plain HTTP admits only localhost, 127.0.0.1, and
[::1] under the explicit test policy.

Headers own wiping byte values, including public values, so an incorrectly annotated sensitive header
still has a wiping lifetime before validation rejects it. Known sensitive names are always redacted.
Arbitrary custom headers can be explicitly sensitive and adopt a move-only SecretValue. Requests
never format URLs or bodies. Responses own wiping body bytes because authentication responses can
contain tokens, and their diagnostics never format body or header values. Errors contain only typed
categories and optional numeric codes.

Memory sources adopt owned buffers, including move-only secret buffers without an extra copy. File
sources open only bounded regular files matching the accepted canonical path, size, and timestamp.
They reject symlink/reparse entries and verify both the open handle and named path before returning
the final bytes. Reads are incremental and never allocate the declared file size.

ExactLengthBody captures the source length once, caps each pull to the remaining declared bytes,
contains source panics, checks cancellation around reads, and probes EOF before releasing the final
bytes. Early EOF, excess bytes, invalid counts, and source errors fail permanently. A failed or
cancelled source cannot later appear complete. The EOF probe uses wiping memory in case the source
contains credentials. Empty destination spans do not advance or complete a source.

## Verification and remaining work

Portable tests cover method/default validation, hostile URLs and loopback bypasses, header framing
and byte limits, sensitive redaction, source metadata panics, body/timeout/response caps, owned
streaming, exact-length failures, cancellation, Unicode paths, file growth, truncation, and changed
timestamps. Unix additionally rejects symlink substitution. Native Windows tests use actual file
metadata and the same identity comparison used by discovery and history.

This slice does not execute network requests. Multipart generation, bounded response collection,
static libcurl/Schannel integration, callback containment, loopback transport tests, and native/Wine
TLS and packaging gates remain required by ADR 0005 and the HTTP transport contract.
