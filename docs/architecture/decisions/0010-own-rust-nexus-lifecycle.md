# ADR 0010: Own the Rust Nexus lifecycle boundary

- Status: Accepted
- Date: 2026-10-16

## Context

The Rust migration needs a real DLL lifecycle proof before provider migration. The local
CarrotQuickness dependency pins nexus-rs revision `091a2b1cb3bf7607302f1ea8647a280225fa62e0`.
That revision supports API v6 and uses arcdps-imgui 0.8.0 with ImGui 1.80. The cached ImGui public
header matches the header at the C++ addon's ADR 0006 pin byte for byte.

The binding's convenience export initializes host globals through OnceLock and keeps them after
its deinit function. Cleanup actions run after the addon unload function and in insertion order.
MannyUploader's contract requires explicit admission, reverse deregistration, callback draining,
and release of host state before unload returns.

Some fields in the Rust AddonApi type are non-null function or COM pointer types, although an
incomplete C host table can contain nulls. Borrowing or cloning the entire table would assert
validity of unused fields before they have been checked.

## Decision

Keep the portable package's unsafe-code prohibition. Add a separate Windows x64 cdylib crate in
the Cargo workspace for FFI, with unsafe-operation and safety-comment checks. Pin nexus-rs to the
reviewed revision and record transitive dependencies in Cargo.lock. Link the Windows MSVC static CRT.

Use the binding's ABI types and raw ImGui functions without its global initialization, export,
render, or automatic cleanup helpers. Read only required API slots through raw pointers, interpret
nullable function slots as Option, validate them, and copy function pointers into owned host state.
Copy supplied UTF-8 paths into PathBuf values during load. Retain neither the API table nor textures.

Export GetAddonDef with the existing metadata and a development description. Initialize the
host-supplied ImGui context and allocators before rendering. The shell shows a clear development
message and exposes no upload or credential actions.

Use a closed-by-default atomic callback gate. Lifecycle operations are serialized. Render callbacks
try to acquire a snapshot and return immediately if lifecycle state is unavailable. An admission
guard accounts for each running callback, including during panic unwinding. The drain waiter uses
a condition variable with a bounded timeout so render callbacks never acquire the waiter's mutex.
The gate has no channel allocation left behind when the DLL is unloaded.

Record resource registrations and reverse them on failed load or unload. Null texture results mean
pending GPU work and do not prevent shortcut registration. Optional texture/shortcut failure leaves
the required render and input resources active. Close admission before deregistration and wait for
admitted callbacks before releasing runtime state and clearing ImGui host pointers.

Contain Rust panics inside native entry points. Suppress panic payload output during the loaded
session and report only a static host diagnostic. Restore the previous panic hook before unload
returns. Abort-on-panic builds are rejected because they cannot provide this containment.

## Verification and consequences

The existing native smoke host remains the independent C++ ABI consumer using ADR 0006's headers.
Its standard mode is unchanged. An optional lifecycle-proof mode adds invalid APIs, synchronous
callbacks during registration/deregistration, duplicate load/unload, callbacks after unload, visible
window rendering, and reloading without remapping the DLL.

Portable tests check admission, draining, repeated cycles, and unwind cleanup. A native Rust fixture
injects registration and texture failures and verifies reverse rollback, safe panic diagnostics,
hook restoration, keybind press/release semantics, and subsequent reload.

The shell has no application workers yet. It does not establish provider cancellation, credential,
history, TLS, or full UI parity. Windows MSVC Debug/Release evidence is required alongside later
Wine and Linux checks. Packaging and release symbols remain a later migration gate.
