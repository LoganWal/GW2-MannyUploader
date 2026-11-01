# ADR 0029: Own explicit DonBot aggregate actions in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

DonBotAggregateDeliveryController owns one bounded command slot, one active request, the aggregate
delivery port, and a session-only snapshot. The application owner submits commands and advances it
with the current upload coordinator, configuration service, and verified DonBot snapshot. Render
callbacks will use the runtime's value-command queue. No storage, HTTP, or job mutation occurs in
this controller.

Commands carry two through one hundred unique stable job IDs and the ordinary configuration and
DonBot verification revisions shown when the action was requested. Dispatch checks both revisions,
current verified endpoint and guild, capabilities, guild policy, aggregate bounds, and authorized
route. Refreshing authorization temporarily makes the action unavailable. Guild defaults require
aggregate-default availability. An explicit authorized channel works independently of automatic
per-log delivery and summary message kinds.

The owner re-resolves selected IDs from current coordinator snapshots immediately before dispatch.
Evicted jobs, missing completed fight IDs, duplicate fights, and IDs outside positive signed-64-bit
range reject the command. Receipt guild provenance does not restrict the target guild. DonBot owns
server-side authorization, including legacy receipts and fights uploaded through another guild.

Polling consumes one result per tick. Results correlate by monotonic request ID. Stale results
cannot finish current work. Normalized counts are retained for display, while inconsistent receipt
counts, outcome, or selected-fight count settle as ambiguous. A worker-reported credential failure
remains typed. No result alters per-log provider status, history, or receipts.

Every terminal state requires another explicit command to send again. The controller never retries.
A panic while enqueueing may occur after acceptance, and a panic while awaiting an accepted result
may hide a delivery, so both settle as ambiguous. A rejected enqueue or failure before dispatch
settles as failed. Snapshot text is static and cannot contain adapter panic payloads.

Shutdown clears queued and active correlation, cancels the owned port once, clears receipts, and
rejects later commands. The production delivery worker joins on destruction. Admission reserves
enough snapshot revisions for queueing, dispatch, completion, and final shutdown without wrapping.

## Verification and remaining work

Tests use a real upload coordinator with fake providers and an owned fake aggregate port. They cover
current revision and policy checks, legacy and cross-guild receipts, explicit routes, malformed and
duplicate selections, eviction and Reupload between submit and dispatch, stale results, bounded
admission, result counts, session-only state, no automatic retry, dispatch/poll panics, credential
diagnostics, and idempotent shutdown. No public service receives requests.

The UI selection presenter and runtime command queue must still connect to this owner. Twitch,
native runtime/UI composition, active-work DLL unload, Wine parity, packaging, symbols, and hosted
release validation remain open.
