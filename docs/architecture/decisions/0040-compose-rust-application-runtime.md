# ADR 0040: Compose one Rust application owner

- Status: Accepted
- Date: 2026-10-16

## Decision

ApplicationRuntime owns the polling/ingestion pipeline, options workflows, recent actions, aggregate
delivery, history store, shared configuration, clock, public Twitch identity port, and external-action
launcher. Its public handle exposes only immutable copied snapshots and bounded value commands.
The application layer does not depend on native Nexus, ImGui, concrete provider workers, or UI models.

Startup restores persisted records and seeds all retained identities before discovery. Only the
configured recent tail becomes coordinator rows. Initial configuration reaches existing source,
parser, and provider ports. Protected saved DonBot verification starts when a guild is retained.
Protected Twitch restoration starts when a public application identity is configured. Missing saved
credentials are handled by the existing workflows, and no plaintext fallback is introduced.

The runtime receives the packaged public Twitch Client ID from composition. An ordinary nonempty
override takes precedence. A settings application hook runs outside locks before options processing,
after each command, and after workflow polling. It updates the shared HTTP client's identity before
a later Connect command can dispatch in the same tick. Failure leaves subsequent commands queued
and records a safe error. The client port performs no HTTP, and normal settings validation still
restricts Client ID changes to disconnected/error states.

Live settings update source, stability, parser capacity, history limit, per-provider parallelism,
and captured provider policy through their existing owners. Each successfully applied component is
tracked separately. A failure retries the unapplied component without repeating earlier successful
updates. Selection proposals publish only after source reconfiguration succeeds. Root/cutoff/stability
and polling-interval changes request an immediate scan while preserving accepted identity dedupe.

Provider/parser results and due retries drain on owner ticks between filesystem scans. The existing
application pump supplies this bounded drainage without observing candidates again. Filesystem scans
follow the configured monotonic interval. DonBot refresh uses a five-minute monotonic schedule and
resets after completion, including a refresh admitted and completed in one tick.

History saves compare current coordinator records against the last successful write. A failed write
is retried before allowing more parser/provider dispatch or explicit log actions. Newly accepted
identities are therefore persisted before their metadata results can dispatch uploads on a later
tick. Receipt changes are saved even when a subsequent filesystem scan fails. A pending scan error
remains visible between scans until a successful scan resolves it.

The runtime command handle admits one pending aggregate command, with bounded unique stable IDs and
displayed revisions. The aggregate controller resolves current authorization and retained jobs at
execution. Recent actions retain their independent bounded queue. Rendering never sees mutable
controllers, credentials, persistence, or HTTP clients.

ApplicationRuntimeThread owns one named application thread. It ticks at a 50-millisecond cadence and
waits interruptibly between ticks. Shutdown closes command admission, requests stop, cancels all
owned work, persists normalized final history, stops authentication workflows, and closes configuration.
Joining the application thread also completes destruction and joining of its owned worker adapters.
No thread is detached. Normal shutdown retains protected sessions rather than disconnecting accounts.

## Validation and remaining integration

Deterministic integration tests cover accepted-identity persistence, result drainage between scans,
write-failure backpressure, hidden-history restoration, no replay on toggles/mode changes, same-tick
Client ID ordering, application failure recovery, source/selection updates, periodic refresh,
render-safe actions/aggregate admission, cancellation, and joined application-thread shutdown.

Three failing-before-fix regressions cover constructor panic containment, immediate refresh completion
deadline reset, and scan errors disappearing before a successful scan. All fixtures are synthetic and
use fake asynchronous ports without provider traffic or real user configuration.

The native DLL still uses its development lifecycle shell. Native composition must create these
dependencies with protected storage, Schannel HTTP, filesystem notifications, and concrete workers,
then attach the runtime thread and immutable UI models. Packaged public identity selection, native
options/list rendering, live quick-access updates, clipboard operations, DLL unload with active work,
Windows/Wine parity, and complete release packaging gates remain required before cutover.
