# Twitch authentication-workflow contract

This contract defines the application-owned boundary between Nexus options, the synchronous Twitch
client, protected session persistence, and secret-free broadcaster connection state. It builds on the
wire contract in [`twitch.md`](twitch.md).

## Protected session format

`TwitchOAuthSession` is one opaque, atomically replaceable protected record. Version 1 uses an
independently validated little-endian binary payload containing, in order:

- fixed eight-byte `MNYTWSN1` magic;
- unsigned 32-bit format version `1`;
- signed 64-bit access-token expiry as positive Unix seconds;
- unsigned 32-bit lengths for access token, refresh token, user ID, and login;
- unsigned 32-bit scope count;
- the access token, refresh token, user ID, and login bytes; and
- for each scope, an unsigned 32-bit length followed by its bytes.

Version 1 requires exactly one scope, `user:write:chat`. Tokens must satisfy the Twitch client's
visible-ASCII and 8 KiB per-value limits. User ID and login use the same canonical rules as token
validation. The complete payload must fit the protected store's 16 KiB plaintext limit. Missing,
trailing, overflowed, truncated, invalid, or future-version data is rejected generically without
including any payload bytes in the error.

The outer protected-record envelope supplies record identity, length, CRC, and DPAPI protection. The
inner format deliberately stores the token pair, expiry, identity, login, and granted scope together:
a crash can expose either the complete old session or the complete new session, never a new rotating
refresh token paired with stale access-token metadata.

## Worker command boundary

`ITwitchAuthenticator` accepts one move-only, request-ID-correlated command for exactly one network
primitive: Device Code start, Device Code poll, access validation, refresh, or revocation. Results
return the same request ID and operation. Pending polling returns the device code to its single owner;
failed validation or refresh returns the unmodified credential set when it remains usable for a
retry. Secrets are never copied into snapshots or error data.

The bounded production worker owns one joined `std::jthread`, FIFO request/result queues, output
backpressure, cooperative cancellation, and exception containment. It performs no sleeping,
scheduling, persistence, retry, refresh chaining, or UI mutation. Each command maps exactly once to
the synchronous `ITwitchClient` method of the same operation.

## Controller states and scheduling

`TwitchAuthenticationController` owns public workflow policy on the application thread, while
`TwitchSessionOwner` is the single serialized owner of credential state shared with chat delivery.
The controller has these visible states:

- `Disconnected`;
- `Starting`;
- `AwaitingUser`;
- `Validating`;
- `Refreshing`;
- `Connected`;
- `Disconnecting`;
- `Error`; and
- `ShuttingDown`.

Snapshots may contain only the authenticated login, Device Code user code and activation URI,
authorization/access expiry timestamps, safe diagnostic, revision, and shutdown flag. User ID,
device code, access token, refresh token, raw protected record, credential presence, response body,
and server exception text are excluded.

The controller checks credentials out of the owner in an exclusive, revisioned transaction. The owner
makes chat leases unavailable until that transaction is committed or discarded, so an in-flight
validation or refresh cannot race a chat-side `401` recovery or persist an older token generation.
The controller uses an injected monotonic clock for Device Code polling, authorization expiry,
validation cadence, and retry delays. It uses an injected system clock only to create and display
absolute access-token expiry. Polling never occurs before Twitch's returned interval. Connected
sessions validate at least hourly and refresh within five minutes of known access expiry. Only one
command is in flight.

## Connection and startup ordering

A new connection starts Device Code authorization without changing settings or protected storage.
After start succeeds, the controller publishes only the user code, activation URI, and expiry. Each
pending poll returns the move-only device code to the controller for the next scheduled poll.

An initial token grant is validated before it becomes durable or visible as connected. After exact
client/scope/identity validation, persistence occurs in this order:

1. Encode the complete version-1 session.
2. Atomically replace `TwitchOAuthSession`.
3. Publish `Connected` with the validated login and access expiry.

Startup loads and decodes the protected record, but does not publish its stored identity. It first
validates the access token. A `401`/reconnect result permits exactly one refresh path; a second
reconnect result after refresh requires a new Device Code connection. Successful startup validation
may rewrite the complete session with the server's current login and remaining expiry before
publication.

## Refresh rotation

Scheduled refresh and reconnect recovery move the complete credential set to the worker. If refresh
fails transiently, the returned old set is retained in memory and retried no earlier than the bounded
delay. An invalid refresh grant requires reconnection.

After refresh succeeds, the replacement pair is immediately encoded with the already validated
identity and atomically persisted before any connected publication or chat use. The new access token
is then validated. A changed user ID is rejected as a session-identity violation; a changed valid
login is persisted as a complete session update before publication. Failure to persist the rotated
pair prevents its use and requires reconnection because the server may already have invalidated the
old refresh token.

Chat-side recovery uses the same owner transaction. A rejected older revision receives the already
committed newer generation without refreshing again. Recovery of the current revision validates,
performs at most one refresh, persists the replacement pair before validating it, and commits only
when the authenticated user ID still matches the broadcaster bound to the session.

## Disconnect and shutdown

Explicit disconnect first durably disables Twitch posting in ordinary settings. If that save fails,
remote revocation and local erasure are not attempted. The controller then performs one best-effort
access-token revocation off-thread and erases `TwitchOAuthSession` regardless of the revocation
result. A failed remote revocation is visible but must not retain local credentials. A failed erase is
visible while Twitch remains durably disabled.

An unpersisted Device Code flow may be cancelled locally while it is waiting between polls. Shutdown
clears transient public and secret state, cancels queued/in-flight work cooperatively, joins through
the worker owner, and rejects later commands.

## Deterministic tests

Tests cover every session field and corruption boundary, exact worker operation mapping, secret
return on retryable poll/validate/refresh failures, queue/result backpressure, exception containment,
cancellation, polling cadence and expiry, initial and saved connection, one refresh recovery path,
hourly validation, pre-expiry rotation, atomic store-before-publication ordering, persistence
failures, identity mismatch, disconnect ordering, stale results and leases, serialized
controller/recovery races, secret-free snapshots, and shutdown during recovery.
Default tests never contact Twitch.
