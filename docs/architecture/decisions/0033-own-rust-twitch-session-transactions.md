# ADR 0033: Serialize Rust Twitch session changes

- Status: Accepted
- Date: 2026-10-16

## Decision

The application owns TwitchSessionOwner, shared by the authentication workflow and chat delivery.
An exclusive transaction moves the complete session out of active state. While it is open, chat
lease acquisition and competing recovery return Retry. Transaction IDs reject delayed writes,
and successful protected persistence advances the process-local session revision before publication.
Loading a saved record does not publish its identity or grant a chat lease.

The owner uses ConfigurationService for every protected load, store, and erase. The lock order is
session state followed by configuration. Callers must release configuration guards before entering
the owner. Neither lock crosses HTTP or clock callbacks. Protected operations belong on application
or worker threads. Render callbacks consume separately published immutable snapshots.

Chat receives a wiping access-token lease with the bound broadcaster ID and revision. It receives
no refresh token or protected-store access. A rejected older revision can acquire the current lease
without another network request only when the broadcaster still matches. Zero, future, foreign-user,
and mismatched current-token leases cannot initiate recovery.

Current-generation recovery validates once and permits at most one refresh. A successful rotation
saves the complete replacement pair with its existing broadcaster identity before validating the
new access token. Replacement validation must confirm that same broadcaster. The final complete
session, including updated login and expiry, is saved before publication. Failed rotation persistence
prevents use of either generation. Replacement validation failure clears active state and attempts
protected erasure.

A transient pre-rotation failure can restore the unchanged checked-out active session without
writing or advancing its revision. New and saved sessions cannot use this restoration path. Starting
rotation persistence permanently removes restoration permission, including when storage fails.
Restoration is an application-internal contract whose caller must return the unchanged session.

Shutdown invalidates pending transactions and clears active memory. Delayed network results cannot
persist or publish afterward. The runtime must cancel and join its calling workers. Adapter panics
are contained and become typed errors without retaining their payloads. Public connection snapshots
will be built by the authentication controller and must exclude the owner's internal user ID.

## Verification and remaining work

Deterministic tests cover saved-state privacy, failed and panicking persistence, exclusive checkout,
stale transaction rejection, restoration restrictions, revision handoff, forged and foreign leases,
complete rotation ordering, transient recovery, identity/scope rejection, replacement failure,
erasure failure, cancellation after rotation, lock release during HTTP, and shutdown during recovery.

Authentication scheduling, chat delivery, UI/runtime composition, active-work DLL unload, Wine,
packaging, symbols, and hosted release validation remain pending.
