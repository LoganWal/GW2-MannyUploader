# ADR 0013: Bound Rust ZIP metadata before decompressing EVTC

- Status: Accepted
- Date: 2026-10-16

## Context

The Rust metadata decoder needs a `.zevtc` reader with ADR 0002's resource limits, CRC validation,
Unicode paths, and cooperative cancellation. General-purpose ZIP readers can allocate metadata from
untrusted directory counts before application limits are applied. The cached zip 9.0.3 reader also
searches for earlier directory-end records after a metadata error, which complicates a strict
application preflight around it.

## Decision

Implement the narrow ZIP container boundary from
[PKWARE's published format specification](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT).
Decode integer fields explicitly from bounded slices. Locate an end record whose comment reaches
the physical file end, validate single-disk directory ranges and counts, then bound directory storage
before allocation. Validate entry lengths, ZIP64 extra fields, selected local metadata, data ranges,
and optional signed or unsigned data descriptors. Preserve the existing EVTC and extensionless-entry
selection rules, including case-insensitive `.evtc` names and duplicate rejection.

Support stored and deflated entries, including ZIP64 sizes and offsets within application limits.
Reject encryption, unsupported compression, split disks, malformed metadata, overlapping selected
data/directory ranges, incomplete deflate streams, trailing compressed bytes, and CRC/size mismatch.
Entries remain in memory and are never extracted to filesystem paths. Archive executable prefixes
with adjusted offsets may work, but offset repair and general-purpose ZIP recovery are not provided.
Central-directory signatures and encryption are outside this narrow reader.

Pin flate2 1.1.10 with only its Rust backend and crc32fast 1.5.2. Cargo.lock fixes miniz_oxide 0.9.1,
adler2 2.0.1, simd-adler32 0.3.10, and cfg-if 1.0.4. The inflater handles DEFLATE, including end-of-stream
validation. The adapter independently verifies exact consumed input, output length, and CRC. These
dependencies preserve the Rust 1.85 minimum and add no native DLL or project-owned unsafe code.

Retain the default 256 MiB archive, 768 MiB output, 16 entries, 1 KiB filename, and 200:1 expansion
limits. Read compressed data through a bounded file range and a 64 KiB buffer. Check cancellation
before I/O, between entry scans, and before every output chunk. Validate discovered file size before
reading and reject size or modification-time changes detected during extraction. Errors contain
only typed local categories, never file paths or inflater diagnostics.

## Verification and consequences

Synthetic tests exercise stored/deflated ZIP and ZIP64, both descriptor forms, small final sizes with
ZIP64 local descriptors, entry selection, every truncated prefix, CRC corruption, invalid headers,
hostile counts/offsets/sizes, incomplete deflate streams, Unicode filesystem reads, and cancellation.
Deterministic mutations probe the container parser with strict memory limits. No test uses private
logs or external services.

The C++ miniz adapter and release path remain unchanged. The Rust reader now feeds the portable
metadata decoder. Asynchronous metadata admission and application runtime composition remain the
next integration step. Full Windows/Wine and existing-data migration gates are still required.
