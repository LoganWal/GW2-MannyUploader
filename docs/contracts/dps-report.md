# dps.report upload contract

This is an independently written version-1 contract for uploading a completed arcdps `.zevtc` to
dps.report. It is based on the public dps.report API documentation and observable response shapes, not
on implementation code from the previous uploader.

Verified 2026-08-20 against:

- <https://dps.report/api>
- <https://dps.report/>

## Request

Send one synchronous request from a provider worker, never a Nexus render or options callback:

```text
POST https://dps.report/uploadContent?json=1&generator=ei
Accept: application/json
Content-Type: multipart/form-data; boundary=<generated boundary>
```

The multipart body contains:

1. an optional text field named `userToken`; and
2. one file field named `file`, filename `upload.zevtc`, content type
   `application/octet-stream`, containing the exact stable file bytes.

The API permits parameters in either the query or form data. The user token goes in form data so it
never enters a URL. It is treated as a password: at most 256 visible ASCII bytes, stored only in the
protected credential store, copied only into wipe-on-destruction request memory, and absent from
ordinary settings, errors, diagnostics, and test output.

The file source must still match the canonical identity accepted by discovery. Construction rejects a
missing, non-regular, differently sized, or differently timestamped file. The source rechecks size and
last-write time before returning its final bytes. Multipart framing and the file are pulled
incrementally and the complete body is never assembled in memory.

The request uses a 10-second connect timeout and 15-minute operation and stalled-transfer timeouts.
The unusually long stall allowance follows the API warning that report generation holds the request
open and can take up to 15 minutes under exceptional load. Response headers are capped at 64 KiB and
the JSON body at 1 MiB.

The transport never follows redirects. The primary endpoint is fixed in code; alternate service
domains are not selected automatically because that could replay a token and file across origins.
Support for the documented HTTPS alternate `b.dps.report` requires an explicit future provider policy
and tests. The HTTP-only alternate is rejected by the production transport.

## Successful response

Every 2xx response must be JSON. Unknown fields are ignored for forward compatibility, but these are
required with the documented types:

- non-empty `permalink` using HTTPS and the `dps.report` or `b.dps.report` authority;
- `encounter.success` boolean;
- `encounter.bossId` integer in the unsigned 16-bit range; and
- non-empty UTF-8 `encounter.boss` of at most 256 bytes.

Optional mode fields are interpreted in this order:

1. `isLegendaryCm == true` produces `LCM`;
2. `isCm == true` produces `CM`;
3. positive `emboldened` produces `Emboldened N`; and
4. otherwise mode is empty.

The documented response may contain a non-null `error` while still generating a report. A valid
permalink and encounter therefore remain success, with a generic warning diagnostic; raw server text
is not forwarded to logs or UI.

An absent or empty response `userToken` does not erase an existing credential. A non-empty valid token
is returned separately as a move-only secret so the application owner can atomically persist it before
considering credential rotation complete. A non-empty invalid token is ignored and produces only a
generic warning. A returned token equal to the current credential requires no replacement. Tokens
never enter `DpsReportResult` or a UI snapshot.

Malformed, oversized, wrong-typed, invalid-UTF-8, untrusted-permalink, or incomplete 2xx JSON is a
permanent protocol failure. JSON and server error text are never copied into the diagnostic.

## Status and transport classification

HTTP status is classified by the provider, not the common transport:

| Condition | Classification | Suggested delay |
| --- | --- | ---: |
| 2xx with valid report | Success | — |
| 408 | Retry | 30 seconds |
| 429 | Retry | numeric `Retry-After`, otherwise 60 seconds |
| 5xx | Retry | 30 seconds |
| Other 3xx/4xx | Permanent failure | — |

Only a decimal `Retry-After` from 1 through 900 seconds is accepted. Dates, duplicates, invalid values,
and larger values use the default.

Cancellation is terminal for the attempt and maps directly to `Cancelled`. Timeout, name resolution,
connection, TLS, send, and receive transport failures are retryable with a 30-second suggestion.
Invalid requests, changed/unreadable body sources, oversized responses, unsupported environments,
initialization failures, protocol failures, and internal failures are permanent for that job attempt.
A bounded provider worker may impose a maximum attempt count above this classification.

## Deterministic tests

The client is tested with an injected fake `IHttpClient`; the default suite never uploads a log. Tests
assert:

- exact method, endpoint, public headers, timeouts, and limits;
- token absence from URL/headers and presence only in multipart body;
- streamed multipart framing and exact file bytes;
- file identity changes, early EOF, cancellation, and multipart length validation;
- success, success-with-warning, mode formatting, and optional token return;
- missing/wrong/oversized fields, malformed JSON, and untrusted permalinks;
- every HTTP and transport classification, including bounded `Retry-After`; and
- no token, response body, server error text, URL query, or source path in returned errors.
