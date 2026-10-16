# ADR 0027: Run DonBot uploads and aggregate delivery in Rust workers

- Status: Accepted
- Date: 2026-10-16

## Decision

DonBotProviderWorker captures validated, normalized ordinary options in a private request envelope.
Later configuration changes affect later admissions. An empty guild is allowed only for an idle
configuration without Discord delivery and cannot admit uploads. Invalid updates preserve the
previous configuration. A same-job report selects permalink import, otherwise the worker selects
direct archive upload. WvW remains available to DonBot.

The worker loads DonBotGw2ApiKey from the protected store immediately before every attempt and
releases the store mutex before entering the client. Missing keys, poisoned storage, adapter failures,
and panics cannot expose credentials or start network work with an unavailable key. Keys remain
move-only values and are destroyed before results enter the output queue. The worker never writes or
erases protected records.

Explicit ManualRetry and Reupload requests capture no Discord route. A regression first reproduced
ManualRetry retaining the configured route, then verified suppression for both explicit intents.
This preserves the C++ coordinator's user_initiated_retry behavior. The coordinator retains intent
across scheduled retries, so a subsequent transient retry cannot re-enable Discord delivery.

Successful ingestion retains upload/fight IDs, normalized Discord counts, and captured guild
provenance. The worker checks returned IDs, receipt consistency, and captured delivery intent before
publishing success. Discord skipped, failed, partial, and ambiguous outcomes retain successful
ingestion and expose static typed notices. Missing credentials and unconfirmed upload/delivery expose
typed failure notices. The coordinator owns the static text and keeps failed attempts settled without
automatic retry. Providers cannot inject server text into these details.

DonBotAggregateWorker uses a separate one-thread WorkerPool. It admits one queued or active request
and rejects new requests until an existing result is consumed. Requests have stable IDs and bounded
unique fight IDs. Results are application-owned session values, with normalized delivery counts and
optional typed credential failure. They never mutate per-log upload status or receipts.

Aggregate processing tracks whether the client has been entered. Cancellation before dispatch is
cancelled. Transport errors, timeout/rate-limit/server statuses, malformed successful responses, and
cancellation after dispatch are ambiguous. Client panics or contained internal failures after
dispatch are also ambiguous because delivery may already have happened. Storage failures and panics
before dispatch fail without calling the client. No aggregate path schedules a retry.

Valid aggregate receipts with any ambiguous count produce an ambiguous result. A receipt with sent
messages and no ambiguity succeeds, including partial delivery. Skipped/failed output without sent
messages fails. Invalid counts, outcomes, or mismatched fight counts settle as ambiguous without
retaining the invalid receipt. The domain's existing count-consistency rule now accepts an internal
maximum parameter, preserving four-message upload receipts and permitting one hundred aggregate
outputs.

Both wrappers inherit bounded queues, output backpressure, contained exceptions, cooperative
cancellation, and joined destruction from the shared pool. No new thread scheduler is introduced.

## Verification and remaining work

Worker tests cover both upload routes, per-attempt key loading, lock release before network work,
captured configuration, explicit delivery suppression, protected-store failures and poison,
retry bounds, typed notices, malformed receipts, aggregate single-operation admission, result
backpressure, status/transport ambiguity, post-dispatch panics, receipt counts, cancellation during
key loading, pending-work cancellation, and joined clients. Coordinator tests retain successful
ingestion under ambiguous Discord delivery and prove typed failures do not schedule retries.

The configuration and aggregate action controllers must still authorize current guild/channel
selections and connect these workers to commands and immutable snapshots. Native runtime/UI
composition, active-work DLL unload, Wine parity, packaging, symbols, and hosted gates remain open.
Tests use fake clients and protected-store adapters. No live Discord messages or uploads are sent.
