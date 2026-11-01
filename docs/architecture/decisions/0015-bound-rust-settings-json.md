# ADR 0015: Bound Rust settings JSON at the adapter boundary

- Status: Accepted
- Date: 2026-10-16

## Context

The Rust implementation must preserve the version-1 settings schema and its strict rejection of
unknown keys, including credentials. JSON concerns must remain outside the domain. Atomic file
storage and protected credentials need separate adapters and compatibility evidence.

## Decision

Pin Serde 1.0.228 and serde_json 1.0.154 in Cargo.toml and resolve transitive versions in Cargo.lock.
Use their MIT license option and record the JSON dependency notices. Keep the JSON parser's default
128-level recursion limit. Do not enable unbounded recursion, raw values, or arbitrary precision.

Use [Serde remote definitions](https://serde.rs/remote-derive.html) inside `config` to serialize and
deserialize the domain settings without adding Serde derives or dependencies to domain source.
Explicit default functions reuse each domain settings type. Require schema_version at the top level
and [deny unknown fields](https://serde.rs/container-attrs.html) at every object level. Generated typed
decoding also rejects duplicate fields. API behavior was checked against the pinned source and
upstream documentation on 2026-10-16.

Reject documents above 64 KiB before parsing. A bounded JSON shape pass requires named objects for
the root and nested settings, because Serde's general struct support also admits positional arrays.
Discard that temporary value before decoding the original bytes through the typed definitions.
Decoding the original bytes preserves duplicate-key rejection. Reject malformed UTF-8, comments,
null or wrongly typed fields, trailing content, unsupported schemas, and invalid domain values.

Normalize legacy DonBot channel overrides before validation. The C++ loading behavior also clears
explicit overrides while DonBot is disabled. A regression test reproduced the missing disabled
case in the first Rust value-model slice before the normalization was corrected.

Serialize validated values through a size-limited writer and emit deterministic two-space pretty JSON
with a final newline. Public errors contain only typed categories and static validation field names.
Discard Serde's detailed errors so keys, values, local paths, or accidentally supplied secrets cannot
enter diagnostics.

`JsonSettingsStore` implements the application-owned settings port over `FileStore`. Loading tries
primary then backup, reports the primary failure category when recovery succeeds, and returns defaults
only when both files are absent. Existing invalid files produce typed failures with no silent reset.

Saving validates and serializes before storage access. A valid previous primary is written unchanged
to the backup before primary replacement. Invalid or oversized documents never replace a valid
backup. Read failures stop saving, and backup-write failure prevents primary replacement. A failed
primary replacement leaves the previous primary and its newly confirmed backup available.

## Consequences

Tests cover all field round trips, defaults, legacy normalization, exact document bounds, deep
nesting, malformed Unicode, duplicate keys, nulls, positional arrays, comments, trailing content,
and secret-like unknown keys at every level. The domain remains standard-library-only.

The codec itself has no filesystem access. The store composes the atomic file adapter from
[ADR 0016](0016-own-rust-atomic-file-storage.md). Fault-injection tests cover read, backup-write, and
primary-replacement failures. Native Windows integration exercises backup recovery through the real
adapter on extended-length Unicode paths. Protected credentials and upload-history compatibility
remain separate migration work. Production settings are unchanged.
