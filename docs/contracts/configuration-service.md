# Configuration service contract

This contract defines the application-owned boundary between persisted ordinary settings, protected
credentials, and future Nexus/UI composition. The service is synchronous, has one owner, and is
constructed and destroyed outside render callbacks.

## Ownership and initialization

`ConfigurationService` exclusively owns one `ISettingsStore` and, when available, one
`ISecretStore`. Construction loads ordinary settings exactly once before returning:

- a primary, backup, or default load becomes revision `1` of the immutable configuration snapshot;
- backup recovery source and its safe diagnostic remain visible in that snapshot; and
- failure to obtain any valid ordinary settings prevents service construction.

Protected storage has different startup semantics. An available adapter is owned by the service. A
platform or adapter initialization failure does not discard otherwise valid ordinary settings;
instead, construction records one of these secret-free capability states:

- `Available`;
- `UnsupportedEnvironment`; or
- `InitializationFailed`.

The capability snapshot contains only the state, typed store error, safe diagnostic, and optional
numeric system code. Under Wine, the expected state is `UnsupportedEnvironment` as required by the
protected-credential contract.

Null adapter ownership is rejected unless it is paired with an explicit protected-storage failure.
The service never silently substitutes an in-memory or plaintext secret store.

## Snapshot and write-through behavior

Every `ConfigurationSnapshot` is a deep value containing:

- validated ordinary `Settings`;
- settings load source and optional recovery diagnostic;
- protected-storage capability;
- a monotonically increasing revision; and
- shutdown state.

It never contains credential values, token prefixes, API-key prefixes, credential presence, account
names derived from credentials, or caller-provided secret text.

Saving ordinary settings is write-through. The service first asks `ISettingsStore` to durably save the
candidate and updates its snapshot only after success. A failed save preserves the complete previous
snapshot. After success, load source becomes `Primary`, recovery diagnostics are cleared, and the
revision advances once.

Credential load, replace, and erase operations are routed directly through the owned `ISecretStore`.
Secret values are returned only from the explicit load operation as move-only `SecretValue` objects.
Credential operations do not change the ordinary configuration snapshot.

## Errors and shutdown

Service errors contain an operation category, safe message, and only the relevant typed settings or
secret-store metadata. Settings validation errors and paths may be propagated. Secret errors may
contain a stable credential ID and numeric operating-system code, but never credential bytes or
derived substrings.

Shutdown is synchronous and idempotent. The first call marks the snapshot as shutting down and
advances its revision. Later calls do nothing. Once shutdown begins, settings saves and all protected
credential operations fail as `ShuttingDown` without touching either adapter. Snapshot reads remain
available for teardown and diagnostics. Destruction performs no detached or background work.

## Deterministic tests

Tests use owned fake adapters and cover initialization from each load source, fatal settings-load
failure, available/unsupported/failed protected storage, write-through success, failed-save snapshot
preservation, secret operation routing and error redaction, idempotent shutdown, and post-shutdown
adapter isolation.
