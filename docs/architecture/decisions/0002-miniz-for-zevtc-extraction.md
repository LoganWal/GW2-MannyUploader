# ADR 0002: Use pinned miniz for `.zevtc` extraction

- Status: Accepted
- Date: 2026-08-19

## Context

The addon needs to extract one EVTC payload from a `.zevtc` ZIP archive. The implementation must fit
in one Nexus DLL, validate corrupt data and CRCs, reject excessive expansion before allocation, permit
cancellation during decompression, work with Unicode Windows paths, and behave consistently under
Wine. The dependency must be pinned and permissively licensed.

Candidates considered:

- **miniz 3.1.2:** MIT, one amalgamated C source/header release, no external runtime dependency,
  iterative extraction, CRC validation, and explicit reader callbacks.
- **minizip-ng 4.2.2:** mature streaming API, but a larger configurable dependency graph and more
  build options than this one-entry reader requires.
- **libzip 1.11.4:** mature and feature-rich, but brings a larger library plus compression-library
  integration for a much broader archive feature set.

## Decision

Use miniz 3.1.2 from its official release asset:

- URL: <https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip>
- SHA-256: `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`
- Upstream release: <https://github.com/richgel999/miniz/releases/tag/3.1.2>

Compile it statically into the core with archive writing, compression, zlib compatibility, stdio, and
time APIs disabled. Keep inflate, CRC-32, and ZIP reading enabled.

The adapter opens files through C++ `std::ifstream` constructed from `std::filesystem::path` and gives
miniz an offset-based read callback. It does not use miniz's narrow-character filename APIs. Extraction
uses the iterator API in fixed chunks, checks cancellation between chunks, and requires iterator
finalization to succeed so size and CRC validation complete.

Initial limits, configurable in the archive reader but conservative by default:

- archive file: 256 MiB;
- uncompressed EVTC entry: 768 MiB;
- central-directory entries: 16;
- entry filename: 1 KiB; and
- uncompressed/compressed ratio: 200:1.

Exactly one non-directory `.evtc` entry is required, matched case-insensitively. Other entries are
ignored within the entry-count limit. Encrypted or unsupported entries are rejected.

## Consequences

- The release remains a single DLL with no zlib or ZIP companion DLL.
- CRC and malformed-deflate handling rely on a pinned, fuzz-tested upstream implementation.
- Builds need network access on a fresh dependency cache; CMake verifies the release SHA-256.
- Cancellation latency is bounded by one 64 KiB extraction chunk plus the current inflater call.
- ZIP64 parsing remains available, but application size limits prevent oversized extraction.
- A future dependency upgrade requires updating this ADR, the pinned hash, and the corrupt-archive
  contract suite.
