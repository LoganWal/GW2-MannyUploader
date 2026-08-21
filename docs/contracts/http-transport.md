# HTTP transport contract

This contract defines the provider-independent synchronous HTTP boundary used by dps.report,
GW2Wingman, DonBot, and Twitch. Provider workers may call it; Nexus render and options callbacks may
not.

## Request model

Version 1 supports `GET`, `POST`, `PUT`, `PATCH`, and `DELETE`. A request owns:

- one absolute URL;
- an ordered list of headers;
- an optional, single-use pull body with an exact declared byte length;
- connect, whole-operation, and stalled-transfer timeouts; and
- response header/body limits.

The body source fills caller-provided bounded spans. It never exposes a native file handle or library
callback type. A source must return between one byte and the requested capacity until its exact length
is exhausted. Early EOF, excess bytes, a source error, or a thrown exception fails the request. Retries
and redirects construct a new request and body source; the transport never rewinds a source.

Hard limits are:

| Item | Limit |
| --- | ---: |
| URL | 4 KiB |
| Header count | 64 |
| Header name | 128 bytes |
| Header value | 8 KiB |
| Total request headers | 32 KiB |
| Request body | 512 MiB |
| Buffered response headers | 64 KiB |
| Buffered response body | 16 MiB |
| Connect timeout | 60 seconds |
| Whole operation | 15 minutes |
| Stalled transfer | 15 minutes |

Per-request response limits may be lower than the hard limits and default to 64 KiB of headers and
1 MiB of body. Timeouts must be positive; the default is 10 seconds to connect, 2 minutes overall, and
30 seconds without byte progress.

Header names use the HTTP token character set. Values are printable ASCII plus horizontal tab and may
not contain CR, LF, or NUL. Callers cannot supply `Host`, `Content-Length`, `Transfer-Encoding`,
`Connection`, or `Expect`; the adapter owns framing and connection behavior.

Production requests require HTTPS, reject URL user information and fragments, and always verify the
certificate chain and hostname. Plain HTTP is available only through an explicit adapter test policy
and only for `localhost`, `127.0.0.1`, or `[::1]`. It is not a user setting.

## Redirects and status codes

The transport never follows redirects automatically. Every completed HTTP response, including 3xx,
4xx, and 5xx, is returned as a typed response. A provider may inspect `Location`, validate its scheme,
origin, and provider-specific path, then construct a new request with a fresh body source. This keeps
authorization headers and upload bodies from crossing an untrusted redirect boundary.

HTTP status is not itself a transport failure. Retry and provider success classification remain in the
provider/application layer.

## Cancellation and timeouts

`IHttpClient::execute` is synchronous and receives a `std::stop_token`. It checks cancellation before
opening a connection and while pulling request bytes, receiving headers/body, and reporting transfer
progress. Cancellation returns `Cancelled`, never a partial response. Provider workers must stop and
join before the client is destroyed.

Connect timeout bounds name resolution/connection establishment as supported by the adapter. The
operation timeout bounds the complete transfer. The stall timeout aborts when byte progress remains
below one byte per second for its duration. Timeout errors are distinct from explicit cancellation.

## Responses and errors

A successful transport result contains a status code, ordered response headers, and a bounded byte
body. Duplicate response headers remain separate. Header and body limits are enforced while receiving,
before unbounded allocation. A response that crosses either limit fails as `ResponseTooLarge` and its
partial bytes are discarded.

Transport failures distinguish invalid request, body source, cancellation, timeout, name resolution,
connection, TLS, send, receive, response limit, protocol, unsupported platform, initialization, and
internal errors. They contain a safe static diagnostic plus an optional numeric backend code. They do
not contain request bodies, response bodies, header values, full URLs, URL queries, or source paths.

## Secret redaction

Every header has `Public` or `Sensitive` metadata. `Authorization`, `Proxy-Authorization`, `Cookie`,
`Set-Cookie`, `X-Api-Key`, and `X-Gw2-Api-Key` are always treated as sensitive regardless of caller
metadata. Request validation rejects a known sensitive header marked public. Diagnostic formatting
may include a header name but replaces every sensitive value with `[REDACTED]`.

Sensitive header values overwrite their owned strings on destruction. The libcurl adapter also
overwrites each assembled line after insertion and walks the complete curl header list to overwrite
its copies before freeing them. This narrows normal plaintext lifetime but does not claim protection
from allocator, dependency, operating-system, or crash-dump copies.

Provider contracts must prefer headers or bodies over secret URL queries. When a public API requires a
secret query parameter, the transport still treats the entire query as undisplayable and never places
the URL in an error.

## Production adapter requirements

The selected adapter must:

- ship inside the single addon DLL;
- support Windows and the maintained Wine environment;
- pull large upload bodies without buffering them in full;
- support `PATCH` and arbitrary content types;
- keep automatic redirects, cookies, netrc credentials, and verbose wire traces disabled;
- restrict protocols to HTTPS in production;
- verify certificates and hostnames without custom insecure overrides; and
- contain every C callback exception boundary.

Deterministic tests cover request validation, header redaction, exact body length, response limits,
error mapping, redirect non-following, cancellation, and adapter lifetime. Live public services are not
contacted by the default suite.
