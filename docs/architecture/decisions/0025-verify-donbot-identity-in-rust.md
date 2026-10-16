# ADR 0025: Verify DonBot identity in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

The application owns DonBot verification requests, results, authorized guild/channel values, and the
asynchronous DonBotVerifier port. The synchronous DonBotIdentityClient port belongs to the provider
adapter. Keeping identity verification separate from uploads lets the verification worker depend
only on the operation it executes. The remaining DonBot HTTP operations will share DonBotClient.

Verification validates and normalizes the configured HTTPS base and validates the protected key
before dispatch. It sends the key only in a wiping JSON body to the guild endpoint, with the frozen
five-second connect and thirty-second operation/stall limits. The body allocates its complete size
before copying secret bytes and moves directly into the streaming source. No key-bearing temporary
String or header is created. Endpoint validation retains DonBot's rejection of a trailing-dot
authority even though the general HTTP transport permits one.

The response decoder bounds the document to 256 KiB, guilds to 256, channels to 256 per guild and
2,048 overall, names to 256 UTF-8 bytes, and identifiers to canonical positive signed-64-bit decimal
values. A count-only pass checks the total channel budget before allocating channel names and IDs.
Typed bounded lists reject excess elements before materializing another record. The shared JSON
validator preserves strict Unicode, object shapes, known-field uniqueness, and forward-compatible
unknown fields. Unknown capability strings are inspected without retaining them.

Summary and aggregate capabilities control which delivery policy is published. Aggregate support
requires summary support. Advertised policies require exact booleans, authorized unique channels,
known unique message kinds, and aggregate limits from two through one hundred. An empty message-kind
list remains a valid server policy. Missing capability produces the default unavailable policy.
The client does not persist verified account, guild, or channel data.

DonBotVerificationWorker uses one thread in the existing bounded WorkerPool. The private processor
interface now permits exclusive access to an owned request so a move-only candidate key can move
into a successful result without copying. Job/attempt identity and existing upload behavior remain
unchanged in the other adapters. The verification processor takes the key before invoking the client,
so a failure or panic destroys it before waiting for result queue space. A success returns the same
allocation for later protected persistence. Stable request IDs remain available for panic outcomes.

All non-cancellation failures, including retry suggestions, become a single typed verification
failure. The worker performs no retry or persistence. Cancellation clears pending requests/results,
requests cooperative stop, and joins the client before destruction completes. The future application
controller owns correlation, single active verification, save-before-store ordering, authorization
refresh, and disconnect.

## Verification and remaining work

Fake HTTP tests cover exact JSON and secret placement, timeouts, normalization, capability gating,
names and identifier limits, missing/wrong/duplicate fields, per-guild and global channel budgets,
Unicode, unknown fields, retry categories, cancellation, and contained panics. A regression first
reproduced the trailing-dot endpoint mismatch. Worker tests verify key allocation identity, stable
request IDs, no retry, invalid admission, FIFO/backpressure, cancellation, and joined shutdown.

DonBot upload/import/aggregate operations and its configuration controller remain pending. This
adapter is not yet composed into the native DLL. No tests use an installed credential or contact
DonBot. Native runtime/UI composition, active-work unload, Wine parity, packaging, symbols, and
hosted release checks still gate the complete migration.
