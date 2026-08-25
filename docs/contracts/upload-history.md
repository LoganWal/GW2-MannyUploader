# Persistent upload-history contract

The addon persists enough settled job state to suppress accidental delivery replay across game
restarts. This is ordinary local state, not a secret store and not a remote source of truth.

## Data and bounds

`upload-history.json` is a versioned UTF-8 JSON document in the Nexus addon data directory. It stores
at most 10,000 unique log identities, ordered oldest to newest. Each identity contains the canonical
UTF-8 filesystem path, exact size, and last-write timestamp. A record may also contain detection time,
parsed encounter metadata including optional remaining boss health, public dps.report result, public
GW2Wingman permalink, DonBot upload/fight IDs, normalized Twitch receipt, and every provider's state,
attempt count, and locally generated detail.

OAuth tokens, DonBot keys, Device Codes, request bodies, response documents, account secrets, and
retry deadlines are never stored. The file is capped at 32 MiB before parsing and after serialization.
Paths and strings are UTF-8 validated and bounded; enum values, numeric receipts, duplicate identities,
schema mismatches, and malformed JSON are rejected.

Writes use a sibling temporary file, flush, and atomic replacement. Merging updates records by exact
path/size/write-time identity, preserves older records not present in the bounded in-memory UI, and
trims only the oldest records above capacity.

## Restore and replay policy

Startup restores only the configured recent-log row limit into the coordinator, but seeds file
discovery with every retained identity. Restoring a record never dispatches provider work. A state
that was `Waiting`, `Active`, or `RetryScheduled` at shutdown becomes `Failed` with an interrupted
session detail and no retry deadline. Settled states and receipts remain visible.

Consequently, changing provider enablement, re-enabling the addon, restarting the game, or switching
between New and Today does not upload or chat an unchanged retained identity again. A file with a
different size or write time is a new identity and follows normal stability checks. The only replay
paths are the explicit user actions defined in [`recent-log-actions.md`](recent-log-actions.md).

An invalid previous history document is ignored with a visible recovery diagnostic rather than
trusted as job state. The New selection cutoff still prevents pre-session files from being submitted
at startup. The invalid file is replaced only when valid current state is next persisted.

## Verification

Portable tests cover full receipt/state round trips, Unicode paths, merge replacement, oldest-first
capacity trimming, invalid-document recovery, interrupted-state normalization, non-dispatching
coordinator restore, explicit replay, and discovery seeding. Windows/Wine tests exercise the same
adapter through native filesystem path semantics.
