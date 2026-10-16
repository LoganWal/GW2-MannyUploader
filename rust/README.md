# Rust development

The C++ addon remains the shipping implementation. The Rust workspace contains the portable core
and a Windows x64 development DLL that exercises Nexus lifecycle and rendering. The development DLL
has no log discovery, uploads, credentials, or persistence.

The root package forbids unsafe code. `rust/addon` contains the native boundary and pins Nexus
bindings with the matching ImGui 1.80 dependency. See
[ADR 0010](../docs/architecture/decisions/0010-own-rust-nexus-lifecycle.md).

The portable application layer now owns a coordinator and clock/provider ports. Deterministic tests
cover dps.report fanout, direct uploads, WvW exclusion, bounded history, retry deadlines, attempt
correlation, atomic result validation and replay admission, restore, and cancellation. Requests retain
Reupload intent across retries, and ambiguous delivery requires an explicit action. See
[ADR 0011](../docs/architecture/decisions/0011-correlate-rust-upload-attempts.md).
The provider pool now supplies bounded FIFO queues, result backpressure, live parallelism from 1
through 32, cooperative stop tokens, and joined destruction. Its processors return typed outcomes
without access to mutable jobs. See
[ADR 0012](../docs/architecture/decisions/0012-own-rust-provider-worker-pools.md).
The EVTC decoder reads bounded revision-1 wire records, validates POV account UTF-8, preserves the
last valid boss-health update, and checks cancellation during scans. Synthetic tests cover truncated
prefixes, hostile counts, invalid accounts, and encounter-agent matching. The archive reader supports
bounded stored/deflated ZIP and ZIP64 entries, CRC validation, Unicode filesystem paths, and chunked
cancellation. Its dependency and container boundary are recorded in
[ADR 0013](../docs/architecture/decisions/0013-bound-rust-zevtc-extraction.md).
The asynchronous metadata parser shares the bounded queue core with provider workers. Ingestion
checks parser capacity before creating jobs, correlates metadata results, retains unexpected dispatch
failures, and cancels both parser and upload work on shutdown. Pending metadata remains protected from
history eviction even when every destination is disabled. The portable poller and application pump
now connect bounded file stability and deduplication to metadata ingestion. Scans handle missing
roots, incomplete traversal, Unicode paths, cutoff changes, and removal reconciliation. Parser
saturation defers candidates, and rejected admission releases their dedupe identities. See
[ADR 0014](../docs/architecture/decisions/0014-own-rust-discovery-and-polling.md).
Native directory notifications now cache complete observations with a three-failure polling fallback.
The native handle has creation, modification, rename, deletion, and root-recovery tests. Session
selection fixes Show New at load time and limits rolling discovery cutoff changes to once per minute.
Persistence and runtime scheduling remain separate work. The coordinator and workers are not
connected to the development DLL yet.

The settings domain now carries version-1 defaults, numeric/path/identifier validation, provider
dependencies, and explicit legacy Discord-channel normalization. One shared Twitch template parser
serves settings validation and same-report rendering, with byte and Unicode code-point limits.
The bounded JSON codec preserves version-1 defaults and rejects unknown or duplicate keys, invalid
UTF-8, non-object settings, and oversized documents. Serde remains inside the configuration adapter
under [ADR 0015](../docs/architecture/decisions/0015-bound-rust-settings-json.md). Bounded atomic file
storage now supplies flushed sibling-file replacement, interrupted-write cleanup, and native
long-path tests under [ADR 0016](../docs/architecture/decisions/0016-own-rust-atomic-file-storage.md).
The settings store now validates before writing, preserves a valid previous primary as its backup,
reports recovery explicitly, and returns defaults only when both files are absent. Fault-injection
and native Windows filesystem tests cover recovery ordering. The history codec preserves the existing
MSVC timestamp format, ordinary Windows path prefixes, and normalized receipts. A synthetic probe
verifies complete C++ to Rust to C++ record compatibility. Bounded decoding rejects malformed history
and normalizes interrupted work without dispatch. See
[ADR 0017](../docs/architecture/decisions/0017-preserve-rust-upload-history.md).
Atomic history merging now preserves hidden rows, trims only the oldest records, and reports recovery
without rewriting invalid files during load. Startup restoration seeds all retained identities while
displaying only the configured recent rows. Fault-injection, replay-suppression, and native long-path
tests cover the composition. The development DLL still needs its application runtime connected.

Protected credentials now have move-only, redacted, wiping values and application-owned storage
ports. The bounded protected-file adapter preserves existing envelopes and filenames. Its native
DPAPI adapter exposes Windows versus Wine protection mode and wipes Windows allocations before
release. Native round-trip, tamper, locked-file, erasure, and C++ interoperability checks pass. Wine
execution and runtime configuration/UI composition remain outstanding. See
[ADR 0018](../docs/architecture/decisions/0018-preserve-rust-protected-credentials.md).
The application configuration service now owns both stores, preserves recovery and protection mode
in secret-free snapshots, commits settings only after durable writes, and isolates adapters after
shutdown. Native tests reopen settings and a synthetic protected credential through this service.
See [ADR 0019](../docs/architecture/decisions/0019-own-rust-configuration-service.md).

The HTTP application port now validates bounded requests, HTTPS authorities, explicit loopback test
policy, framing headers, sensitive annotations, and timeouts. Wiping headers and response buffers
keep credentials out of diagnostics. Memory/file sources stream incrementally, and a length guard
contains source failures and verifies exact EOF. File bodies recheck accepted identity before their
final bytes. See [ADR 0020](../docs/architecture/decisions/0020-bound-rust-http-requests-and-bodies.md).
Multipart bodies stream bounded fields using platform-generated boundaries. Response collection
preserves ordered headers and trailers, counts interim responses toward limits, and discards partial
data on failure or cancellation. See [ADR 0021](../docs/architecture/decisions/0021-stream-rust-http-framing.md).
The native HTTP adapter builds the pinned static libcurl/Schannel backend and executes requests
through contained callbacks. Deterministic loopback tests cover streaming, cancellation, limits,
redirects, and TLS rejection of plaintext. See [ADR 0022](../docs/architecture/decisions/0022-own-rust-native-http.md).
The dps.report client streams stable archives and protected token fields, validates bounded report
responses, and classifies retry suggestions. Its bounded worker captures Detailed WvW per request
and saves replacement tokens before publishing success. Typed warnings reach coordinator snapshots.
See [ADR 0023](../docs/architecture/decisions/0023-upload-to-dps-report-in-rust.md).
The Wingman client now supports same-job permalink import, direct bridge uploads, bounded ticket
polling, and public fight-link discovery. Its worker preserves queued/duplicate notices and receipts
while sharing bounded queues and joined cancellation. Fake time verifies polling deadlines without
public uploads. See [ADR 0024](../docs/architecture/decisions/0024-upload-to-wingman-in-rust.md).
DonBot verification now decodes bounded authorized guilds, channels, and delivery capabilities. Its
single-thread worker transfers the candidate key into a correlated successful result without copying,
retrying, or persisting it. See [ADR 0025](../docs/architecture/decisions/0025-verify-donbot-identity-in-rust.md).
The DonBot client also supports TUS uploads, trusted permalink imports, completion streams, and
explicit aggregate delivery. TUS failures after creation cannot request automatic retries, and
Discord receipts must match the captured request. See
[ADR 0026](../docs/architecture/decisions/0026-upload-to-donbot-in-rust.md).
DonBot upload and aggregate workers now capture ordinary settings, load protected keys per attempt,
and retain typed receipts and safe notices. Explicit retries suppress per-log Discord delivery.
Aggregate delivery permits one outstanding operation and never retries automatically. See
[ADR 0027](../docs/architecture/decisions/0027-run-donbot-workers-in-rust.md).
DonBot configuration and aggregate-action controllers now enforce verified guild and route choices,
persist changes before publication, and correlate asynchronous results. Twitch HTTP primitives,
authentication workers, protected sessions, and exclusive session recovery are implemented. See
[ADR 0033](../docs/architecture/decisions/0033-own-rust-twitch-session-transactions.md) for rotation
ordering and shared credential ownership. Twitch authentication scheduling, chat delivery, and
production runtime composition remain pending.

## Portable core

```sh
cargo fmt --all --check
cargo clippy --locked -p manny-uploader --all-targets -- -D warnings
cargo test --locked -p manny-uploader
cargo test --locked -p manny-uploader --release
```

## Windows MSVC

Requires Rust with the `x86_64-pc-windows-msvc` target, Visual Studio 2022 C++ Build Tools,
and CMake 3.30 or newer.
The target configuration links the static CRT. Builds and tests use the workspace lockfile.
Cargo builds the pinned libcurl source in its output directory. The first native build downloads
the hash-verified release archive. For an offline native build, set `MANNY_CURL_ARCHIVE` to a local
copy of that archive. Cargo's `--offline` flag governs Rust dependencies, so set this archive override
as well when avoiding CMake downloads. `MANNY_RUST_CMAKE` selects the CMake executable.
Cross builds can provide `MANNY_RUST_CMAKE_TOOLCHAIN` and `MANNY_RUST_CMAKE_GENERATOR`.
Debug Rust builds use RelWithDebInfo for libcurl to match Rust's release static CRT while retaining
native debug symbols. Release builds use Release for both.

```sh
cargo clippy --locked --workspace --all-targets --target x86_64-pc-windows-msvc -- -D warnings
cargo test --locked --workspace --target x86_64-pc-windows-msvc
cargo test --locked --workspace --target x86_64-pc-windows-msvc --release
cargo build --locked -p manny-uploader-addon --target x86_64-pc-windows-msvc
cargo build --locked -p manny-uploader-addon --target x86_64-pc-windows-msvc --release
```

The default suite uses local fake servers. To run the public Schannel HTTPS probe explicitly:

```sh
cargo test --locked -p manny-uploader-addon --target x86_64-pc-windows-msvc --release live_schannel_https_probe -- --ignored
```

Build the existing CMake `manny_nexus_addon_smoke_test` target using the same CMake installation that
configured its build directory. Pass the Rust DLL path to that executable. Run both modes for each
configuration, for example in PowerShell:

```powershell
$hostPath = 'out/build/native-msvc/tests/Debug/manny_nexus_addon_smoke_test.exe'
foreach ($configuration in @('debug', 'release')) {
    $dll = "target/x86_64-pc-windows-msvc/$configuration/manny_uploader.dll"
    & $hostPath $dll
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $hostPath $dll --lifecycle-proof
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
```

The standard mode performs ten load/render/unload/FreeLibrary cycles. The extra mode checks invalid
API rejection, duplicate load/unload, callbacks during registration/deregistration and after unload,
visible-window rendering, and two load/unload sessions per DLL mapping. Both alternate pending and
immediate texture creation. These tests run in a separate process without Guild Wars 2.

Rust tests additionally exercise partial registration rollback, optional texture failure, panic
redaction and hook restoration, and draining admitted callbacks. The native shell owns no worker
threads yet. Worker cancellation and joined shutdown must be added with the application runtime.

These checks do not replace Linux/Wine coverage, native credential and HTTP validation, packaging,
or symbol verification. Full Rust release and existing-data compatibility gates remain outstanding.
