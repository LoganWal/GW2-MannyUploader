# ADR 0009: Migrate the addon to Rust incrementally

- Status: Accepted
- Date: 2026-10-16

## Context

The maintainer prefers Rust for ongoing development. The existing behavior contracts, ownership
boundaries, and C++ tests provide acceptance criteria for an incremental migration. CarrotQuickness
provides a local style reference for direct modules, explicit enums, and a small Nexus host adapter.

## Decision

Add one Cargo package alongside the CMake project, with Rust source under `rust/src` and integration
tests under `rust/tests`. Keep the production C++ addon and its release gates operational until the
Rust implementation meets the same behavior contracts. The first package is a portable library,
without an installable DLL or native dependencies.

Preserve ADR 0001's dependency direction and single mutable job owner. Domain code uses only the
standard library. The initial crate forbids unsafe code. Native integration must later isolate and
audit its necessary FFI rather than relaxing safety checks throughout the portable core.

Use a nonzero job ID type and a retry-state variant containing a monotonic deadline. Preserve the
existing provider transitions, receipt rules, failed-provider retry, explicit delivery preparation,
and normalization of interrupted history. The application coordinator will continue to own provider
routing, grouped Reupload admission, and dispatch. The job model performs no I/O.

Reject attempt-count overflow before mutating a job. Validate Twitch receipts on restore as well as
completion, and require dropped messages to have no message ID. This enforces the normalized receipt
contract even for malformed empty or non-printable IDs. These checks deliberately tighten the domain
boundary compared with the C++ implementation. Persistence still owns schema, string, origin, and
resource-limit validation before constructing records.

Represent filesystem paths with `PathBuf` and modification times with `SystemTime`. This is an
in-memory representation only. The persistence adapter must explicitly convert the existing timestamp
format and prove exact identity compatibility before it can read or write production history.

A synthetic fixture produced by the existing Microsoft C++ history writer confirms two important
Windows differences. Microsoft STL persists 100-nanosecond file-clock ticks from the Windows epoch.
Rust filesystem metadata exposes the same native ticks but represents modification time relative to
its SystemTime epoch. Rust canonicalization also adds verbatim drive or UNC prefixes that are absent
from the existing C++ weakly-canonical paths.

LogFileIdentity equality and hashing normalize equivalent verbatim drive/UNC prefixes while retaining
the complete original PathBuf for I/O. This prevents a seeded C++ identity from being treated as a new
Rust log. Size and exact modification time remain part of identity. A regression test first reproduced
automatic replay after two observations and now covers both drive and UNC paths. History encoding
still needs explicit timestamp conversion and rollback-compatible path serialization.

## Consequences

Rust core tests can run independently on Linux and Windows. CI adds formatting, Clippy with warnings
denied, and Debug/Release tests on Linux and Windows MSVC. Local Windows GNU results establish only
that compiler's coverage.

The Rust settings value types and Twitch template grammar also live in the standard-library-only
domain. JSON representation belongs to a separate adapter. Rust strings enforce UTF-8 in memory,
while the future decoder must reject malformed input before constructing these values. Settings
errors carry only static field names and typed categories. Canonical Discord ID validation is shared
with delivery receipts, including rejection of leading zeroes. HTTPS settings validation also rejects
empty hosts, malformed IPv6 authorities, and invalid ports before an HTTP adapter receives them.
Template expansion checks bounds before appending and never truncates a chat message. Settings and
delivery use the same parser, preserving escaped-brace and real-URL-placeholder semantics.

The existing C++ addon remains the shipping implementation. The initial Rust slice has no discovery,
coordinator, provider workers, credentials, persistence adapter, UI, or Nexus entry point.
[ADR 0010](0010-own-rust-nexus-lifecycle.md) adds a separate development lifecycle DLL. Neither slice
alters installed settings or delivery history.

Before replacing the C++ DLL, Rust must prove Nexus/ImGui ABI compatibility, callback draining,
panic containment, cancellation and joined worker shutdown, Windows/Wine parity, existing-data
compatibility, repeated real-DLL smoke cycles, and packaging with usable symbols. Cargo tests alone
do not replace those release gates.
