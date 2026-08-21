# ADR 0003: Use pinned Glaze for JSON settings

- Status: Accepted
- Date: 2026-08-19

## Context

The addon needs a versioned settings file and later needs small provider JSON request/response bodies.
The parser must reject malformed UTF-8 and unknown settings keys, return useful errors without throwing
across native boundaries, work with C++23 on MSVC/GCC/Clang, add no companion DLL, and remain pin-able
with a verified source artifact.

Candidates considered:

- **Glaze 8.0.0:** MIT, header-only, typed direct-to-struct mapping, no-exception error contexts,
  strict unknown-key handling, bounded parsing options, and full UTF-8 validation by default.
- **nlohmann/json 3.12.0:** MIT and extremely established with a convenient dynamic DOM, but heavier
  template/binary cost for a small fixed schema and an exception-oriented primary API.
- **yyjson:** MIT, compact, fast, and non-throwing, but its C DOM requires substantially more manual
  field/type/default mapping and cleanup code for the same fixed C++ settings schema.

Glaze 8.1.0 was current during the spike, but raised its upstream CMake minimum to 3.31. The project
retains its CMake 3.25 baseline, so version 8.0.0 is the newest compatible release selected here.

## Decision

Use Glaze 8.0.0 from its official tag archive:

- URL: <https://github.com/stephenberry/glaze/archive/refs/tags/v8.0.0.tar.gz>
- SHA-256: `569152f5ec43c510b2ec339476e2d0b78066068855e1a91594dbdfafcd7d248d`
- Upstream release: <https://github.com/stephenberry/glaze/releases/tag/v8.0.0>

Link its header-only `glaze::glaze` target privately into the core. Include only JSON headers. Settings
reads keep UTF-8 validation and unknown-key errors enabled, validate trailing input, and impose file and
string limits before domain validation. Writes use deterministic pretty JSON.

The settings domain remains independent of Glaze. Serialization metadata and file I/O stay in the
configuration adapter, so provider/domain code does not expose third-party JSON types.

## Consequences

- JSON adds no runtime DLL and no exception requirement.
- Aggregate settings structs map directly without a hand-written DOM traversal.
- Unknown settings keys fail closed; an older addon will not silently rewrite a newer schema.
- The dependency is deliberately one compatible release behind current because of the CMake baseline.
- Updating Glaze requires rerunning malformed/oversized/UTF-8/corruption tests, reviewing release
  security notes, and updating this ADR, the pinned hash, and third-party notice if needed.
