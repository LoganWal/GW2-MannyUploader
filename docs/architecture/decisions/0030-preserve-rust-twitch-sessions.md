# ADR 0030: Preserve protected Twitch sessions in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

The application owns move-only TwitchCredentialSet and TwitchSession values and the version-1 inner
session codec. The public helpers share token, user ID, and login validation with upcoming Twitch
adapters without introducing a dependency from application code to providers.

Encoding preserves the existing MNYTWSN1 binary format: little-endian version, signed Unix-second
access expiry, four field lengths, scope count, access/refresh tokens, identity and login, and one
length-prefixed user:write:chat scope. Positive expiry is bounded through year 9999 and fractional
seconds truncate as in the current format. Canonical positive user IDs cover the full unsigned
64-bit range. Logins use lowercase ASCII letters, digits, and underscore with a 64-byte limit.

Both tokens require nonempty visible ASCII with an 8 KiB individual limit. The combined record must
fit the protected store's 16 KiB plaintext limit, including framing. Encoding validates every field
before allocation, creates one fully sized SecretValue, and writes directly into its wiping buffer.
There are no intermediate secret Strings, growing secret vectors, or plaintext persistence paths.

Decoding bounds the complete record before reading headers, resolves lengths through checked slices,
and validates every borrowed field before allocating tokens or identity. It rejects truncation,
trailing bytes, overflowed lengths, unknown versions, invalid expiry, and any scope set other than
the one allowed scope. Decoded tokens immediately enter move-only wiping values. Diagnostics are
typed and static. Debug output for the whole session is redacted.

## Verification and remaining work

Tests cover an independent exact binary fixture, all truncated prefixes, every length field, magic,
version, expiry boundaries, fractional seconds, token byte and size constraints, combined maximum
size, identity/login/scope validation, and debug/error redaction.

A local synthetic compatibility probe uses genuine MSVC and the existing C++ codec and DPAPI store.
Rust opens and validates the C++ protected session, re-encodes it byte-for-byte, and atomically saves
it through the Rust protected store. C++ then decrypts and validates the Rust record and compares all
fields. Both directions pass. Probe sources, generated records, and executables stay outside Git in
ignored build output. No installed credentials are read or modified.

This implements persistence values only. Twitch HTTP primitives, authentication scheduling, shared
session ownership, chat recovery/delivery, and native runtime composition remain pending. Wine,
packaging, symbols, active-work DLL unload, and hosted release gates remain open.
