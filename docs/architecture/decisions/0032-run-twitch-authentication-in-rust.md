# ADR 0032: Run Twitch authentication primitives in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

The application owns move-only TwitchAuthenticationCommand values, request/result correlation,
success variants, retained secret ownership, and the TwitchAuthenticator port. Commands represent
exactly one of Device Code start, poll, access validation, refresh, or revocation. A chat request is
not representable through this authentication boundary.

TwitchAuthenticationWorker implements the port with the existing WorkerPool, one thread, bounded
FIFO request/result queues, cooperative cancellation, and joined destruction. It performs one
TwitchApi call per command and does not sleep, schedule, retry, persist, chain refresh/validation,
or mutate public connection state.

A private captured request preserves the original ID and operation while moving its command to the
processor. The command stays outside the client panic boundary. Pending polling returns the same
device-code allocation. Successful validation returns the unchanged credential pair alongside the
new validated identity. Successful polling/refresh returns the new token grant and destroys the old
command secrets before result backpressure.

Failed polling returns its original device code, and failed validation/refresh returns the complete
unchanged credential set. These move-only values live in a dedicated retained field beside the typed
outcome, never in diagnostic text. The controller decides whether to recover, retry later, reconnect,
or discard them. Terminal failures and contained client panics preserve the same ownership rule.
Revocation returns no retained access token. Results always retain the command's operation even if
an injected client reports mismatched error metadata.

All request/result Debug paths redact token values. Client panic payloads become an Internal error.
Cancellation clears pending requests and results, requests stop on the active primitive, and joins
before the production owner can finish destruction. No worker has access to settings or protected
storage.

## Verification and remaining work

Fake-client tests cover all five operation mappings, exact request/result IDs, device and credential
allocation identity, successful rotation, every failure disposition, panic containment and retained
ownership, no automatic retry/chaining/chat, invalid admission, queue/result backpressure, FIFO,
single-thread ownership, cancellation of pending secrets, and joined active work.

The shared session owner and authentication controller must still serialize credential mutations,
persist complete rotations before publication, schedule polling/validation/refresh, and coordinate
chat-side recovery. UI/runtime composition, Wine, packaging, symbols, active-work DLL unload, and
hosted release validation remain pending.
