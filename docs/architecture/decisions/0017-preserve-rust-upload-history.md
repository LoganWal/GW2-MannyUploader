# ADR 0017: Preserve upload history across the Rust migration

- Status: Accepted
- Date: 2026-10-16

## Context

Restart deduplication depends on exact persisted file identity. The shipping Microsoft STL writer
stores Windows file-clock ticks and ordinary drive or UNC paths. Rust uses SystemTime and may return
extended-length canonical paths. A format change could silently submit the same log again or prevent
a rollback to the C++ addon from recognizing Rust history.

## Decision

Keep schema version 1, its field names, numeric enum codes, and optional receipt fields. The codec
uses the shipping Windows timestamp convention on every platform: nonnegative signed 64-bit counts
of 100 ns since 1601. Convert with checked integer arithmetic relative to the Unix epoch. Reject
unrepresentable times and sub-tick modification precision instead of rounding identities. Detection
time remains milliseconds since the Unix epoch with C++ truncation toward zero.

Serialize verbatim drive and UNC paths with their ordinary prefixes on Windows. Keep extended paths
in memory for I/O and use the domain's equivalent-prefix identity comparison. Do not canonicalize
saved paths during decoding because the file may no longer exist. Reject invalid native Unicode
instead of replacing characters.

The configuration adapter owns JSON. A bounded sequence visitor admits at most 10,000 records and
bounded text visitors validate before allocating owned field values. Named-object wrappers reject
positional arrays. The input is capped at 32 MiB before parsing, and a bounded output writer enforces
the same limit while serializing one record at a time. Duplicate identities are rejected using the
same equality and hash rules as discovery.

Reject unknown and duplicate fields, missing required identity/state fields, schema mismatches, and
invalid normalized receipts. Optional metadata and receipts may be absent, including the additive
DonBot guild field. Validate public report origins using the frozen recent-action rules. These
checks deliberately tighten C++ history parsing, which accepted unknown fields and deferred origin
validation. Malformed history must use the visible recovery path rather than become trusted state.

Persist numeric busy states without retry deadlines. Decoding passes interrupted states through
domain restoration, producing a failed state and the existing interrupted-session detail. Neither
decoding nor restoration dispatches work. Validate outgoing records before any persistence operation.

## Verification

A synthetic probe linked against the genuine MSVC C++ core writes all receipt fields and an exact
sub-microsecond modification timestamp. Rust decodes it and compares identity against real native
filesystem metadata, then writes a history file from the extended canonical path. The existing C++
reader accepts that file and compares the complete record equal. The probe and generated files stay
in ignored build output. The checked-in synthetic fixture preserves that wire data with a portable
placeholder path.

Automated codec tests cover both timestamp epochs, signed detection times, Unicode paths, drive and
UNC compatibility, all enum codes, interrupted normalization, legacy optional fields, trusted links,
malformed objects and receipts, duplicate identities, and byte/count limits. Native Windows and Wine
release gates remain required before replacing the shipping DLL.
