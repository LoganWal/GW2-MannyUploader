# ADR 0043: Prove active Rust DLL unload

- Status: Accepted
- Date: 2026-10-16

## Decision

The explicit lifecycle-test Cargo feature substitutes a bounded synthetic HTTP adapter at native
composition. Every other dependency and lifecycle path remains the real addon implementation.
Default builds retain Schannel and contain no test probe exports. The test DLL advertises its
purpose in Nexus metadata and must never be installed in Guild Wars 2 or packaged for release.

The synthetic adapter accepts only a bounded dps.report POST, drains the real multipart body, then
waits for the provider worker's stop token. It performs no network I/O. A thirty-second watchdog
turns missing cancellation into test failure. Read-only test exports report entered/exited calls,
active calls, live shared clients, retained jobs, and application-active dps.report jobs. The host
reads those counters only while the DLL is mapped.

The smoke host's --active-runtime-proof mode requires those exports before calling Load. It uses
fresh isolated settings, enables only dps.report with parallelism two, and creates three valid
synthetic archives after each load. It waits for three application-active uploads and exactly two
calls blocked inside HTTP, proving one additional request is queued. Unload must return within
five seconds with both calls exited, no active calls, no retained runtime, and no live shared HTTP
client. Ten cycles include both repeated sessions in one DLL mapping and FreeLibrary/reload.
Existing registration, callback-admission, render, and reverse-teardown checks remain active.

Normal smoke mode rejects a DLL exposing either test probe. Package verification independently
rejects the lifecycle-test export marker in the DLL. A failing-before-fix package contract fixture
and a real test-DLL archive verify that rejection. CI builds test DLLs into a separate target
directory, runs Debug and Release active-work checks, and verifies normal smoke rejection.
Windows Rust test jobs now explicitly require native DPAPI.

## Scope of evidence

This proves real-DLL application/provider lifetime and cancellation, including queued work, without
using public providers or user credentials. The native Curl adapter's separate loopback tests cover
transport cancellation. It does not prove native/Wine browser or clipboard behavior, hosted CI
execution, all Wine behavior, Rust release packaging/symbol compatibility, or release cutover.
