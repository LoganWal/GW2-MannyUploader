# GW2Wingman upload contract

This contract defines the conditional GW2Wingman integration used by GW2 Manny Uploader. When the
same job enables dps.report, Wingman imports its trusted permalink. Otherwise the addon sends the
`.zevtc` through the existing `evtc.bel.st` compatibility bridge.

Verified 2026-08-25 against:

- <https://gw2wingman.nevermindcreations.de/api>;
- <https://gw2wingman.nevermindcreations.de/uploader>;
- the compatibility bridge client at <https://github.com/belst/nexus-wingman-uploader>; and
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
without changing application scheduling. The direct bridge is the explicit fallback only when
dps.report is disabled for that job.

## dps.report permalink import

When dps.report is enabled, the coordinator leaves Wingman waiting. A successful dps.report result
queues two bounded requests without reading or uploading the archive:

```text
POST https://gw2wingman.nevermindcreations.de/api/importLogQueued?link=<percent-encoded-permalink>
GET  https://gw2wingman.nevermindcreations.de/api/checkLogQueuedOrDB?link=<percent-encoded-permalink>
Accept: application/json
```

The permalink must use the exact `https://dps.report/` prefix and contain no credentials, query,
fragment, backslash, control, or non-ASCII byte. The import response is successful only when its
integer `success` is `1`. The status response must include boolean `inQueue` and `inDB`, at least one
of which is true, plus a `targetURL` under the exact
`https://gw2wingman.nevermindcreations.de/log/` prefix with a safe log slug. The target is retained
immediately so the UI can open the fight while Wingman finishes processing it. `inDB: true` is
reported as duplicate success.

Both requests use 10-second connect, 60-second operation, and 30-second stalled-transfer timeouts.
Headers and bodies use the normal 64 KiB response limits. Redirects are disabled. Transport and HTTP
status classification follows the table below.

If dps.report fails or is cancelled, Wingman is skipped or cancelled with it. An explicit retry of
dps.report re-arms a skipped Wingman attempt. A Wingman retry after dps.report success imports the
same retained permalink again. The idempotent queue endpoint makes that retry safe.

## Direct fallback request

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

## Direct fallback response

A `409 Conflict` means the log is already present and is treated as successful duplicate delivery.
The body is ignored and the client proceeds to permalink discovery.

Every 2xx response other than the duplicate status must be JSON with a boolean `result`. Unknown
fields are ignored for forward compatibility. `true` means accepted. When a positive integer
`ticket` is present, the client polls `GET https://evtc.bel.st/status/{ticket}` every 3 seconds for up
to 30 minutes. `uploaded`, `skipped`, and `404` finish ticket processing. `queued`, `processing`, and
`deferred` continue polling. `failed`, an unknown state, malformed JSON, or an invalid zero ticket is
a permanent failure. A legacy successful response without a ticket remains accepted without a fight
link. Raw response content is never copied into diagnostics.

After ticket completion, and immediately for a duplicate, the client polls
`POST https://gw2wingman.nevermindcreations.de/checkUploadSuccessfulWithLog` every 5 seconds for up
to 5 minutes. Its URL-encoded fields are the bridge filename derived from the POV account, stable
file size, boss ID, and POV account. A successful response must provide an ASCII alphanumeric,
hyphen, or underscore `log.html` slug. The retained public permalink is
`https://gw2wingman.nevermindcreations.de/log/{slug}`. Expiry without a match preserves upload
success without exposing a View fight action.

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
bridge or uploads a log. Tests assert exact multipart fields and streamed bytes, fixed endpoints and
limits, queued permalink import and status resolution, legacy and ticketed success, polling,
permalink validation, duplicate success,
false/malformed/incomplete JSON, every status and transport class, bounded retry headers,
stable-file changes, input validation, cancellation, endpoint policy, exception containment, and
diagnostic redaction.
