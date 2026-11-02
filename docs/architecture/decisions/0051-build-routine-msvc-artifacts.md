# ADR 0051: Build routine MSVC artifacts

- Status: Accepted
- Date: 2026-11-02

## Decision

Build the routine `Build DLL` artifact on `windows-latest` with the Rust MSVC target. Run formatting,
warning-clean Clippy, Release tests with native DPAPI required, package verification, and the
packaged-DLL smoke test before uploading the standalone DLL and versioned ZIP with checksum.
Keep these artifacts named by their source commit.

This supersedes the MinGW routine-build choice in ADR 0047 and the routine-artifact statements in
ADRs 0049 and 0050. The manual `Release validation` workflow remains the authoritative release gate
for Debug and Release native tests, active-provider unload, symbol mismatch checks, and the optional
live Schannel probe. Native in-game validation remains required before publication.

## Reason

The routine artifact is often downloaded for direct testing. Building it with the same native
toolchain as the shipping package removes the GNU runtime and cross-compiler differences observed
while investigating a Defender detection on a published MinGW DLL. A different binary may still
receive an antivirus detection, so this change does not establish a false-positive verdict.
