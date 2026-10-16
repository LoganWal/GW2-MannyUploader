# ADR 0034: Coordinate Twitch authentication in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

TwitchAuthenticationController owns Device Code authorization, saved-session startup, periodic
validation, refresh scheduling, and explicit disconnect on the application thread. It submits one
primitive at a time through TwitchAuthenticator and uses TwitchSessionOwner for protected sessions.
No controller path performs HTTP directly. UI callbacks receive immutable connection snapshots.

A new flow reserves the session owner before starting authorization. The reservation has no
broadcaster identity until validation succeeds and cannot persist a rotation. Polling transfers the
device code to the worker and waits at least the server interval after each response. Retry delays
are positive and bounded, with a 30-second fallback, and cannot shorten the polling interval.
Authorization expiry rejects late grants, including grants from an already running poll.

All scheduling uses monotonic deadlines. System time supplies absolute display and credential
expiry values. Connected sessions validate hourly and refresh five minutes before access expiry.
The controller observes the owner's state atomically so chat recovery cannot appear disconnected
between two separate reads. A committed newer generation updates the public login and schedule.
Restoring an unchanged generation preserves its original deadlines.

Successful initial validation saves the complete session before publishing Connected. Saved and
hourly validation permit one reconnect-driven refresh. The controller saves both rotated tokens
before dispatching replacement validation. Retries retain the returned credential pair under the
same exclusive transaction. Replacement validation cannot trigger a second refresh. Changed
broadcaster identity, invalid rotation, and failed rotation persistence prevent publication and
attempt protected erasure.

Every completion must match the current request ID and operation. Stale IDs leave the current
request intact. Mismatched operations, malformed success variants, unexpected retained secrets,
invalid metadata, and missing retry credentials fail with typed diagnostics. Error values contain
no provider response, local path, token, device code, or server exception payload.

Disconnect first saves Twitch posting as disabled. A failed settings save preserves the workflow
and skips revocation and erasure. After that save, revocation runs once on the worker. Its failure,
dispatch rejection, malformed result, or worker panic still leads to protected erasure. Shutdown
during explicit disconnect also attempts erasure before invalidating the owner and joining the
authentication worker. Failed erasure remains a typed diagnostic while posting stays disabled.
Ordinary shutdown retains the durable session for validated startup on the next load.

## Verification and remaining work

Thirty-one deterministic tests cover polling cadence, monotonic scheduling across wall-clock jumps,
expiry during a poll, startup privacy, complete rotation ordering, bounded retries, initial and
second reconnect rejection, stale and malformed results, metadata bounds, unchanged-generation
deadline preservation, public snapshot privacy, persistence failures, disconnect ordering, panic
containment, unrepresentable deadlines, and shutdown cleanup. The deadline and disconnect cleanup
fixes have regression tests observed failing before their implementation changes.

The shared owner tests also cover recovery serialization and in-flight shutdown. Native runtime
composition must call this controller on the application thread and publish snapshots for UI use.
Chat delivery, options/list UI, active-work DLL unload, Wine, packaging, symbols, and hosted release
validation remain pending.
