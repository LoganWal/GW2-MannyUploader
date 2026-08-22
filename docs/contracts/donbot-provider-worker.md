# DonBot provider-worker contract

`DonBotProviderWorker` connects the synchronous DonBot client to `IUploadProvider` through the shared
`AsyncUploadWorker`. It owns no credential and performs no HTTP, protected storage, or configuration
I/O on a Nexus callback.

## Configuration capture

The wrapper owns a mutex-protected ordinary configuration containing the API base and selected guild
ID. Initial and updated values must pass the same structural HTTPS and positive-decimal guild rules
used at the settings boundary. An enabled DonBot configuration requires a selected verified guild.

`enqueue()` copies the current configuration into a DonBot-only request context before delegating to
the bounded worker. This gives every accepted attempt deterministic settings: a later options save
affects later enqueues but cannot retarget work already waiting in the queue. Other provider wrappers
reject this context, and DonBot rejects pre-populated context or a dps.report dependency result.

The API key is deliberately absent from the request context. The worker loads
`DonBotGw2ApiKey` from `ISecretStore` immediately before every network attempt. Missing storage is a
permanent, actionable configuration failure; read or adapter failure is a generic permanent failure.
The key remains a move-only `SecretValue` and is destroyed after the attempt.

## Result mapping and lifecycle

DonBot success becomes a provider success and carries the optional numeric upload and processed
fight IDs as a typed receipt. The coordinator retains that receipt for persistent history and
aggregate-link composition. Client retry, failure, and cancellation map directly to provider outcomes. Retry
durations must be positive and no more than 24 hours; an absent or invalid duration becomes 30
seconds. No DonBot result carries a dps.report response.

Queue bounds, output backpressure, exception containment, cooperative stop, idempotent cancellation,
and joined destruction are inherited from
[`async-provider-workers.md`](async-provider-workers.md). Escaping secret-store or client exceptions
become a generic DonBot worker failure and cannot expose exception text.

Condition-variable tests cover configuration validation and snapshot timing, secret loading on each
attempt, correct secret identifier, missing/failing/throwing storage, every result type, retry
defaulting, invalid cross-provider payloads, client exceptions, in-flight cancellation, pending-work
removal, and post-shutdown rejection.
