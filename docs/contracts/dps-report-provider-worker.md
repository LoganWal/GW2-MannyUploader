# dps.report provider-worker contract

This contract defines how the synchronous dps.report client is owned and connected to the
single-owner upload application. It covers local concurrency, result delivery, retry scheduling, and
credential persistence; the wire protocol remains in [`dps-report.md`](dps-report.md).

## Ownership and execution

`DpsReportProviderWorker` implements the application-owned `IUploadProvider` port and always reports
`Provider::DpsReport`. Its client and optional protected secret store are borrowed dependencies and
must outlive the provider. The wrapper owns the shared `AsyncUploadWorker`, which provides one
bounded joined thread pool, a bounded FIFO request queue, and an equally bounded result queue. Queue
capacity must be non-zero and defaults to eight; parallelism defaults to one and is configurable from
1 through 32. The common mechanics are frozen in
[`async-provider-workers.md`](async-provider-workers.md).

Each accepted request must have a non-zero job ID and attempt, the dps.report provider ID, a non-empty
stable file path, and no previous dps.report result. Full, stopping, or malformed requests are rejected
synchronously with generic diagnostics. Up to the configured number execute concurrently away from
Nexus callbacks. Results preserve job ID and provider ID so the coordinator can reject stale or
mismatched completion.

Provider threads block when the result queue is full. Taking a result releases that
backpressure. It never creates detached tasks, unbounded queues, or one thread per upload.

## Credential ordering

Before each upload, the worker loads only `SecretId::DpsReportUserToken`. `NotFound` means anonymous
upload; every other load failure fails the attempt before network work begins. Store diagnostics and
exceptions are reduced to generic messages.

If a client response returns a replacement token and protected storage is available, the worker must
atomically store the replacement before publishing a successful upload result. A store failure does
not publish the report as success, preventing the application from claiming that credential rotation
completed. No token, protected-store message, path, or client exception text enters the result.

When protected storage is unavailable, anonymous uploads remain supported. A server-issued token is
discarded through move-only wipe-on-destruction memory and the valid report is returned with a generic
warning. Wine normally supplies its explicitly warned DPAPI compatibility store instead.

The protected store must serialize access to the dps.report record while the worker is alive. Nexus
options will submit credential changes through the eventual application owner rather than calling the
same record concurrently from a render callback.

## Result and retry mapping

The worker maps the client result into `ports::UploadResult`:

| Client result | Upload outcome | Payload |
| --- | --- | --- |
| Valid report | `Succeeded` | report, generic detail, no retry delay |
| Retryable error | `Retry` | safe detail and a positive delay |
| Permanent error | `Failed` | safe detail only |
| Cancellation | `Cancelled` | safe detail only |

A missing or invalid client retry suggestion becomes 30 seconds. The provider returns a duration,
not a time point. The single-owner coordinator validates a positive delay of at most 24 hours and
converts it to a monotonic retry deadline using its injected clock. This keeps scheduling and mutable
job state out of the worker.

The application pump drains a configured maximum number of provider results per tick, applies them by
stable ID using a persistent round-robin cursor so a busy provider cannot starve another, then
dispatches retries whose monotonic deadline is due. Result retrieval exceptions and malformed results
stop the tick with a generic coordinator error instead of corrupting job state.

## Cancellation and destruction

`cancel_pending()` is idempotent. Its first call marks the worker stopping, clears queued requests and
unconsumed results, requests the thread stop token, and wakes every wait. In-flight HTTP and test
clients receive the same stop token. No completion is published after stopping; the coordinator owns
settling active jobs as cancelled. Destruction calls cancellation and the owned `std::jthread` joins
before borrowed dependencies can be destroyed.

## Deterministic tests

Tests use fake client and protected-store ports and assert:

- invalid capacity and request rejection;
- FIFO stable-ID success and exact outcome mapping;
- token load before upload and replacement store before success publication;
- missing, failed, throwing, and unavailable protected storage behavior;
- generic diagnostics without token, store, or exception text;
- retry-delay forwarding and defaulting;
- bounded input queues and output backpressure;
- in-flight cancellation, pending-work removal, late enqueue rejection, and joined destruction;
- bounded application-pump result drainage and coordinator-owned retry deadlines; and
- exception containment at client, store, and provider-result boundaries.
