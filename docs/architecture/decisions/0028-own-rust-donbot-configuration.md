# ADR 0028: Own DonBot configuration in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

DonBotConfigurationController owns the verifier and transient authorization. Each command borrows
the application owner's ConfigurationService, keeping storage ownership available to the other
destination controllers without a second mutable configuration owner. The controller performs no
HTTP. Synchronous persistence runs only on the application owner, outside render callbacks.

Snapshots contain independent owned identity and policy values, the durable selected guild, safe
typed diagnostics, and lifecycle/revision state. They never contain credentials or presence flags.
The verifier receives the move-only candidate key. A successful candidate saves the normalized base
with uploads and delivery disabled and selections cleared, then stores the protected key, then
publishes verified identity. Failed settings persistence cannot replace the old protected record.
Saved-key verification loads through ConfigurationService and never rewrites the returned key.

Only one verification is admitted. Monotonic request IDs correlate results and stale results consume
one bounded poll without finishing current work. Both saved and candidate results are rejected if
the configured endpoint changed during verification. Refresh admission also checks the currently
verified endpoint before loading a key. A regression demonstrated that rebinding an old visible
identity to a newly configured endpoint during refresh could otherwise authorize the old channel.

Refresh retains the last verified controls while blocking mutations. Failed refreshes retain durable
settings and the previous identity and publish a static diagnostic. Successful refreshes sanitize
revoked guilds and routes before publishing. A revoked override disables automatic delivery even
when guild defaults are available. An unmarked stale channel is cleared and resolves to defaults.
Guild changes clear delivery, selecting the same guild preserves it, and disabling automatic
delivery retains an authorized route for explicit aggregates.

Pure authorization helpers require a matching current base, selected guild, verified identity, and
live settings and policy. Automatic delivery additionally requires an enabled message kind. Aggregate
default routing checks aggregate-default availability. Aggregate capability, enabled policy, selection
count, and explicit action correlation belong to the upcoming aggregate action controller.

Disconnect persists disabled settings first, clears identity, then erases the protected key. Save
failure prevents erase. Erase failure leaves uploads durably disabled. Adapter panics become typed
failures without retaining their payloads. Shutdown is idempotent, clears transient state, cancels
pending work, and rejects later calls. The owned production verifier joins its worker on destruction.

## Verification and remaining work

Owned fake tests cover key ownership, normalization, persistence order and failures, stale IDs and
bases, authorization revocation, nondestructive refresh, guild/channel controls, aggregate routes,
adapter panics, shutdown, and secret-free snapshots. No public service or real credentials are used.

The runtime owner must still schedule refresh on enable and at most once per five minutes while
enabled and verified, gate discovery during saved verification, and publish authorized provider
routes. The options presenter owns the ordinary upload enable toggle and its route clearing.
Native runtime/UI composition, Twitch, aggregate actions, active-work DLL unload, Wine parity,
packaging, symbols, and hosted release validation remain open.
