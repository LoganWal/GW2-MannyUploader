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

## Portable core

```sh
cargo fmt --all --check
cargo clippy --locked -p manny-uploader --all-targets -- -D warnings
cargo test --locked -p manny-uploader
cargo test --locked -p manny-uploader --release
```

## Windows MSVC

Requires Rust with the `x86_64-pc-windows-msvc` target and Visual Studio C++ Build Tools.
The target configuration links the static CRT. Builds and tests use the workspace lockfile.

```sh
cargo clippy --locked --workspace --all-targets --target x86_64-pc-windows-msvc -- -D warnings
cargo test --locked --workspace --target x86_64-pc-windows-msvc
cargo test --locked --workspace --target x86_64-pc-windows-msvc --release
cargo build --locked -p manny-uploader-addon --target x86_64-pc-windows-msvc
cargo build --locked -p manny-uploader-addon --target x86_64-pc-windows-msvc --release
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
