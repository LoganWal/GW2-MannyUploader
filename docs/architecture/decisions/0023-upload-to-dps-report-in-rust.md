# ADR 0023: Upload to dps.report through Rust workers

- Status: Accepted
- Date: 2026-10-16

## Decision

DpsReportClient implements a synchronous provider-owned uploader interface using injected HTTP and
random-source ports. It preserves the frozen endpoint, optional Detailed WvW query, multipart fields,
stable-file streaming, 10-second connect timeout, 15-minute operation and stall timeouts, and bounded
responses. User tokens enter only wiping multipart sources. Invalid input, file changes, cancellation,
and random-source failure stop the attempt safely. HTTP exceptions and returned failures expose only
typed categories, status codes, and bounded retry delays.

Response decoding requires object shapes, required field types, bounded UTF-8 encounter names, and
trusted HTTPS report origins. WvW report origins require the WvW boss ID. Queries, fragments,
credentials, backslashes, quotes, controls, non-ASCII URLs, and empty report paths are rejected.
Known duplicate fields fail. Unknown fields remain ignored for forward compatibility.

The bounded document is checked for UTF-8 and paired Unicode escapes before typed deserialization.
This closes serde_json's skipped-string validation gap without retaining unknown values or imposing
numeric ranges on unknown fields. Tests reproduce both an invalid surrogate being accepted in an
unknown field and an otherwise valid unknown large number being rejected by a generic value pass.
The final check validates Unicode escapes only. serde_json retains ownership of structural parsing
and recursion limits. Required field visitors bound allocation before copying accepted strings.

Response tokens copy directly into move-only wiping values. Invalid non-empty tokens produce a
typed warning, absent or empty tokens do not erase credentials, and unchanged tokens need no write.
Server error text is inspected only for a warning and is never retained in results. No JSON error,
provider response text, path, or client exception message enters a diagnostic.

DpsReportProviderWorker uses the same WorkerPool implementation as AsyncUploadWorker. Its private
request envelope captures Detailed WvW on admission, so external callers cannot supply or replace
that context. Stable job and attempt IDs are retained in results. Configuration changes affect later
requests. Capacity, parallelism, backpressure, cancellation, and joined destruction remain shared
mechanics.

The worker retains its client and optional protected store through Arc ownership. A mutex serializes
secret-store calls and fails closed if poisoned. It loads only the dps.report token before network
work and releases the store lock during the upload. Missing storage supports anonymous upload.
Replacement tokens must be durably stored before a success result can enter the output queue. A
save failure produces a failed attempt. Missing protected storage discards the returned token and
publishes the valid report with a typed warning. No plaintext fallback is added.

UploadOutcome now supports typed success warnings. The coordinator supplies their static UI text
while retaining receipt validation and dependent-provider dispatch as one transaction. Workers
cannot inject arbitrary server or credential text into these snapshots.

## Verification and remaining work

Fake HTTP tests cover exact requests, streamed form bytes, both endpoint options, credentials,
response types and limits, Unicode, report origins, encounter modes, token rotation, cancellation,
random-source failure, file changes, every transport classification, and bounded Retry-After values.
Worker tests cover persistence ordering, unavailable and failing storage, poisoned locks, exceptions,
configuration capture, FIFO queues, backpressure, parallelism, retry mapping, cancellation, and join.
Coordinator tests verify visible warnings and dependent dispatch without losing the report receipt.

No test uploads a log to a public service. Native runtime composition, remaining provider clients,
authentication workflows, active-work unload checks, Wine execution, and hosted release gates remain
required before the Rust addon replaces the production C++ addon.
