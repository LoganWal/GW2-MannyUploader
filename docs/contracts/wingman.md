# GW2Wingman compatibility-upload contract

This contract defines the version-1 direct `.zevtc` integration used by GW2 Manny Uploader. The
request is sent to the existing `evtc.bel.st` compatibility bridge, which submits accepted logs to
GW2Wingman without requiring the addon to bundle Elite Insights.

Verified 2026-08-20 against:

- <https://gw2wingman.nevermindcreations.de/api>;
- <https://gw2wingman.nevermindcreations.de/uploader>; and
- observable validation and response behavior at <https://evtc.bel.st/evtc>.

## Compatibility boundary

GW2Wingman's current public API does not document a raw-EVTC upload endpoint. Its documented direct
path requires the original `.zevtc` plus Elite Insights JSON and HTML through `/uploadProcessed`, and
the official uploader currently requires the .NET 8 desktop runtime. The public API separately
supports importing an already processed dps.report link through `/api/importLogQueued`.

This project deliberately does not embed Elite Insights or a .NET runtime. It therefore keeps the
small raw-EVTC bridge as an explicit compatibility integration. The bridge is live but is not a
GW2Wingman public API, so it may change or disappear without notice. Its URL is owned by the client
adapter rather than ordinary settings, and the constructor seam permits a reviewed replacement
without changing application scheduling. The addon must never silently fall back to forwarding the
dps.report permalink because direct Wingman is currently modeled as an independent provider.

## Request

Send one synchronous request from a provider worker, never a Nexus render or options callback:

```text
POST https://evtc.bel.st/evtc
Accept: application/json
Content-Type: multipart/form-data; boundary=<generated boundary>
```

The multipart body contains these parts in order:

1. `account`: the POV Guild Wars 2 account name from the EVTC metadata;
2. `filesize`: the stable archive size in decimal bytes;
3. `triggerID`: the EVTC boss/species ID in decimal; and
4. `file`: the exact stable archive bytes, with filename `upload.zevtc` and content type
   `application/octet-stream`.

The file must be non-empty, the trigger ID non-zero, and the account non-empty valid UTF-8 of at most
256 bytes with no ASCII control characters. The stable file source verifies path, size, and
last-write time before opening and after its final bytes. Multipart framing and file bytes are pulled
incrementally rather than assembled in RAM.

The request has a 10-second connect timeout and 15-minute operation and stalled-transfer timeouts.
Response headers are capped at 64 KiB and the response body at 64 KiB. Redirects are never followed.

## Response

A `409 Conflict` means the log is already present and is treated as successful duplicate delivery.
The body is ignored.

Every 2xx response other than the duplicate status must be JSON with a boolean `result`. Unknown
fields, including the bridge's current queue ticket, are ignored for forward compatibility. `true`
means accepted; `false`, a missing/wrong-typed field, malformed JSON, or trailing content is a
permanent failure. Raw response content is never copied into diagnostics.

## Status and transport classification

| Condition | Classification | Suggested delay |
| --- | --- | ---: |
| 2xx with `result: true` | Success | — |
| 409 | Duplicate success | — |
| 408 | Retry | 30 seconds |
| 429 | Retry | numeric `Retry-After`, otherwise 60 seconds |
| 5xx | Retry | 30 seconds |
| Other 3xx/4xx or invalid 2xx | Permanent failure | — |

Only a decimal `Retry-After` from 1 through 900 seconds is accepted. Dates, duplicate headers,
invalid values, zero, and larger values use the default.

Cancellation maps directly to `Cancelled`. Timeout, name resolution, connection, TLS, send, and
receive errors retry after 30 seconds. Invalid requests, file identity/read failures, oversized
responses, unsupported environments, initialization errors, protocol failures, and internal errors
are permanent. Diagnostics never contain the account, source path, request URL, response body, or
transport message.

## Deterministic tests

The client is tested through an injected fake `IHttpClient`; the default suite never contacts the
bridge or uploads a log. Tests assert exact multipart fields and streamed bytes, fixed endpoint and
limits, valid and forward-compatible success, duplicate success, false/malformed/incomplete JSON,
every status and transport class, bounded retry headers, stable-file changes, input validation,
cancellation, endpoint policy, exception containment, and diagnostic redaction.
