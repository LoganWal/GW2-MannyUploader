# ADR 0035: Deliver Twitch chat in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

TwitchChatDelivery is the shared synchronous policy for encounter posts and explicit test messages.
It depends only on TwitchApi and the application-owned TwitchDeliverySessionAccess port. It never
loads protected storage, refreshes tokens directly, or accepts a configurable broadcaster/channel.
The application owns typed chat outcomes and static diagnostics. Receipt validation remains in the
domain and is shared with the delivery boundary.

A message must fit the 500-code-point and 2,000-byte limits and contain no ASCII controls before
session acquisition. The acquired lease supplies the broadcaster ID and access token. One HTTP 401
permits one owner recovery and one resend of the same message. Recovery must retain the broadcaster
and cannot return an older revision. A second 401 requires reconnection.

Only connection setup failures and an explicit HTTP 429 with retry disposition request automatic
retry. Delays are positive and bounded to 24 hours, with a 30-second fallback. HTTP 408 and 5xx,
timeouts, send/receive failures, malformed receipts, response failures, client panics, and cancellation
after entering the client are ambiguous. They settle without automatic retry. Cancellation before
client entry is safe. Session failures after a confirmed 401 cannot duplicate the rejected message.

TwitchProviderWorker captures the parsed template and result-posting policy at admission. Excluded
reports skip credential acquisition and the ledger. Rendering uses only the same-job report, and
results preserve job, provider, and attempt correlation. A mutex-protected 256-entry FIFO ledger
retains sent receipts and ambiguous keys. Explicit user actions bypass settled entries. An additional
in-flight key set prevents two parallel attempts from sending the same job/permalink concurrently.
That set is bounded by the shared pool's parallelism limit. No ledger lock crosses a client call.

TwitchTestMessageWorker uses one shared-pool thread and formats only the fixed local message plus
its request ID. It strips the remote message ID before returning its typed result. The application
controller accepts a test only while the connection snapshot is Connected and no previous test is
pending. It correlates completions, preserves normalized drop status, and exposes retry suggestions
as errors for an explicit later user action. It never schedules another test automatically.

Both worker adapters use bounded input/output queues, cooperative cancellation, panic containment,
and owned thread handles. Cancellation requests stop without blocking. Destruction joins every
owned thread before releasing client and session dependencies. Render callbacks must not call the
synchronous delivery boundary or blocking test wait helpers.

## Verification and remaining work

Twenty-six deterministic tests cover broadcaster identity, exact message/token use, one recovery,
stale/foreign leases, bounded session and transport retries, HTTP 408 ambiguity, malformed receipts,
all normalized drops, Unicode limits, cancellation stages, panic containment, same-job rendering,
configuration capture, ledger keying/eviction, concurrent duplicate suppression, explicit replay,
test-message uniqueness, result privacy, input/output backpressure, and joined destruction.

Controller tests cover Connected gating, one pending request, stale results, every terminal outcome,
no automatic retry, dispatch/result panics, and idempotent shutdown. A regression observed failing
before its fix proves that idle polling cannot replace a completed test result with an error.

Runtime composition, options/list UI, live configuration wiring, active-work DLL unload, Wine,
packaging, symbols, and hosted release gates remain pending. The native development DLL still owns
only its lifecycle shell.
