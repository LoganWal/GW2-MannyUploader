# ADR 0045: Run MSVC Rust tests under Wine

- Status: Accepted, hosted execution pending
- Date: 2026-10-17

## Decision

Add a Wine CI gate using the exact Windows MSVC test executables and DLLs produced by the native
jobs in the same workflow. This complements the maintainer's local MSVC-under-Wine preflight when
its Linux toolchain is unavailable. Native Windows remains the authoritative operating-system gate.

Cargo's machine-readable no-run output selects every workspace test executable in Debug and Release.
Staging requires a fresh directory and rejects artifacts outside the selected MSVC profile or with
duplicate names. Each configuration has a bounded manifest of executable basenames. Rust doctests
remain covered by the existing native Linux and Windows jobs.

The Wine job uses Ubuntu 24.04's Wine packages and a virtual display. It creates a dedicated fresh
64-bit prefix, removes the native-DPAPI requirement, and requires Wine DPAPI compatibility instead.
Start the persistent wineserver before wineboot. Starting it afterward competes with the server
that wineboot already launched and fails with Wine's lock-conflict exit status 2, before any tests
run. A runner regression models that lock ownership and verifies test execution and cleanup.
See [Wine 9 server lock acquisition](https://github.com/wine-mirror/wine/blob/wine-9.0/server/request.c).
It runs every staged test executable, then both normal DLL smoke modes and the active-provider
unload proof for each configuration. Finally it verifies the already-created Rust ZIP/checksum and
runs both smoke modes against the extracted DLL. Each subprocess has a timeout. Cleanup terminates
only the Wine processes in the prefix created by this run, including after a failed or timed-out test.

The job depends on native Rust tests, the C++ smoke-host job, and verified Windows release packaging.
Test executables and lifecycle DLLs are short-lived CI inputs with one-day retention. No Microsoft
compiler, SDK, import library, or toolchain directory is uploaded. Release symbols remain in their
existing separate artifact. Python tests cover manifest rejection, full configuration/mode coverage,
failure propagation, DPAPI environment selection, and refusal to reuse an existing Wine prefix.

Failed subprocesses emit a public CI annotation with the executable name, exit code or timeout,
and bounded Rust test identifiers when available. Raw process output stays in the ordinary job log,
bounded to one MiB per process and collected on disk. Annotations exclude command arguments,
local paths and raw failure text. This supports diagnosis from public check results without using
stored repository credentials to retrieve logs.

## Evidence boundaries

Successful artifact staging and mocked runner tests do not prove Wine execution. Record the hosted
Wine job result and version before claiming that gate passed. The default suite uses synthetic
credentials and local fake servers. The ignored public HTTPS probe and UI draw export remain opt-in.
This gate does not exercise real browser launch, clipboard integration, provider accounts, or the
in-game Nexus matrix. Those Windows/Wine checks remain required before shipping cutover.
