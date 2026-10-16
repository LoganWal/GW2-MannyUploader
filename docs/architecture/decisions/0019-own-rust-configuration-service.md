# ADR 0019: Own configuration through one Rust application service

- Status: Accepted
- Date: 2026-10-16

## Decision

ConfigurationService owns the ordinary settings store and either a protected credential store or
an explicit initialization failure. A Result argument makes an absent adapter without a reason
unrepresentable. Construction loads settings exactly once and validates them before publishing
revision 1. Failure to load valid ordinary settings prevents construction. Protected-store failure
preserves valid settings and publishes UnsupportedEnvironment or InitializationFailed capability.

The snapshot contains only settings, typed recovery metadata, protected-storage capability, revision,
and shutdown state. Available capability includes NativeUserScoped or WineCompatibility so runtime
composition can carry the Wine warning without probing the platform again. Snapshots are independent
owned copies. Credential presence and values never enter them.

Settings changes validate first and write through the owned store. Only a successful durable save
replaces the snapshot, marks the source Primary, clears recovery, and advances revision. Failure
preserves the complete previous snapshot. Credential operations route through the protected store
without changing the ordinary snapshot. Their errors retain only the requested stable ID, typed
category, and optional numeric system code.

The service has one application owner and synchronous adapter calls. Rust ownership controls mutable
access. Render callbacks consume separately published immutable snapshots, never this service or its
storage adapters. No worker, mutex, or background task is introduced inside the service.

Shutdown is idempotent, advances revision once, and rejects every later adapter operation. Saving
reserves the final u64 revision for shutdown and rejects exhaustion before writing. This avoids
silently accepting a change without a new revision. Destruction relies on owned adapter lifetimes
and starts no work.

## Verification

Deterministic tests cover all settings load sources, recovery, fatal load failure, invalid loaded
settings, explicit unavailable capability, Wine mode, independent snapshots, successful writes,
failed-write preservation, secret routing and redaction, shutdown isolation, and revision exhaustion.
Native Windows tests additionally compose the real JSON and DPAPI file adapters, save settings and
a synthetic credential, reopen both through a new service, and erase the credential. The service is
not yet connected to the development DLL's runtime or UI.
