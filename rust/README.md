# Rust development

The C++ addon remains the shipping implementation. The Rust workspace contains the portable core
and a Windows x64 development DLL that exercises Nexus lifecycle and rendering. The development DLL
has no log discovery, uploads, credentials, or persistence.

The root package forbids unsafe code. `rust/addon` contains the native boundary and pins Nexus
bindings with the matching ImGui 1.80 dependency. See
[ADR 0010](../docs/architecture/decisions/0010-own-rust-nexus-lifecycle.md).

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
