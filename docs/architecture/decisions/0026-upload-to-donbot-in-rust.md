# ADR 0026: Implement DonBot upload protocols in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

DonBotClient now implements the synchronous upload, same-job permalink import, and explicit aggregate
delivery operations. DonBotOptions carries only ordinary endpoint, guild, and delivery-route data.
The application-owned route enum represents none, guild defaults, or one channel override. This
prevents channel IDs from accompanying routes that do not use them. Client validation still requires
canonical positive signed-64-bit guild/channel IDs and a protected key immediately before dispatch.

Direct uploads create one TUS resource with the implementation-owned filename, selected guild, and
wingman=false metadata. Discord route fields are added only when requested. The creation response
requires unique protocol and location headers. Locations resolve only to a safe file ID at the exact
configured origin and TUS path. A requested Discord delivery also requires a unique acceptance
acknowledgement and valid upload ID before PATCH begins.

The stable file opens after creation and streams through the existing identity-checking body source.
PATCH success requires status 204, one matching TUS version, and one canonical offset equal to the
original size. Every non-cancellation transport, status, or handshake failure after creation settles
as unconfirmed without a retry suggestion. Earlier transient creation failures remain retryable.
This preserves the frozen duplicate-prevention boundary without adding resumable-upload state.

Permalink imports require trusted dps.report origins and use the selected guild with the captured
delivery route. Responses require valid upload IDs, duplicate flags, and consistent pending/complete
status and fight IDs. Completed replies validate the normalized receipt against the request. If a
pending reply includes a receipt, it must also match the request intent before the completion stream
is consulted. An idempotent import remains eligible for the existing transient retry classification.

Completion streams use the same sensitive key header, long upload timeouts, and a 256 KiB body limit.
Each data line is decoded through the bounded JSON validator. Explicit failed stages, malformed
events, invalid IDs, and receipts before completion fail permanently. The final data event must be
complete. Requested delivery requires its matching receipt on that final event. An unavailable
optional stream preserves confirmed ingestion without a fight ID. Requested delivery instead settles
as unconfirmed. Cancellation is checked before and after every HTTP phase.

Aggregate requests validate two through one hundred unique positive signed-64-bit fight IDs and send
them as JSON strings. The response must match the selected count and carry a requested normalized
receipt with at most one hundred outputs. The synchronous client makes exactly one request and
retains typed transport/status categories. The future dedicated action worker must classify
post-dispatch uncertainty as ambiguous and must never treat Retry as permission to repeat aggregate
delivery automatically. Aggregate results remain separate from per-log receipts.

Keys enter only wiping sensitive headers. Response-derived diagnostics contain typed categories and
numeric HTTP status/transport information. Domain receipts retain only upload/fight IDs and normalized
Discord counts/outcomes. No response text, Discord message data, path, account, or credential enters
an error or receipt.

## Verification and remaining work

Fake transport tests cover exact requests, all accepted location forms, origin/path attacks, duplicate
headers, both TUS handshakes, streamed bytes, file mutation, delayed file opening, optional progress,
Discord acknowledgement, final-event receipts, normalized outcome/count consistency, permalink
origins and idempotency, aggregate IDs and routes, input/response limits, retry boundaries,
cancellation, and panic containment. No live upload, credential, or Discord delivery is used.

DonBot provider and aggregate workers, configuration/action controllers, and native runtime/UI
composition remain pending. The complete migration still requires active-work DLL unload, Wine parity,
packaging, symbols, and hosted release checks.
