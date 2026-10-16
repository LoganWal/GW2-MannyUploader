# ADR 0014: Own Rust discovery policy and portable polling

- Status: Accepted
- Date: 2026-10-16

## Context

Rust metadata ingestion needs stable, unique filesystem identities without blocking Nexus callbacks.
The frozen [polling contract](../../contracts/log-polling-pipeline.md) defines observation stability,
incomplete-scan retention, exact-identity deduplication, and parser backpressure.

## Decision

Keep observation policy in the standard-library-only Rust domain. `LogDiscovery` requires at least
two matching size/time observations, tracks files independently, and remembers accepted identities
in a FIFO history with a hash index. Identity membership does not refresh eviction order. Changing
the stability threshold clears only pending observations. Complete seed validation precedes mutation.

The application owns `LogCandidateSource` and its value types. `PollingLogSource` implements that
port with Unicode filesystem paths, ASCII extension matching, cutoff filtering before candidate
counting, sorted snapshots, cooperative cancellation, and no background thread. Windows reparse
points, including directory junctions, are excluded. Other platforms exclude symlinks.

Incomplete scans merge retained paths and emit no removals. Missing roots clear retained paths and
emit their removals. Hard errors preserve the previous snapshot. Diagnostic values contain only
static categories, with at most one entry per category, and never contain paths or filesystem errors.
Valid reconfiguration replaces the source state atomically. Invalid reconfiguration preserves it.

Bound candidates, pending stability records, and retained identities to at most 10,000 each. Repeated
incomplete scans cannot accumulate more than the configured candidate bound. Exceeding that bound
fails without mutation. Traversal also limits open directory depth to 128 and directory iteration
steps to 1,000,000, including unsuccessful reads. These explicit Rust limits fail with `ResourceLimit`
instead of allowing unbounded allocation or traversal. Polling keeps one iterator per depth.

`ApplicationPump` owns the source, discovery policy, and ingestion coordinator. Each tick drains
bounded metadata and provider results, dispatches bounded due retries, polls, reconciles removals,
and admits stable candidates only while parser capacity remains. Failed pre-admission releases the
identity. Unexpected parser rejection retains the accepted job. Source panics become typed failures.
Shutdown is idempotent and reaches both metadata and provider workers through ingestion.

`NotifyingLogSource` wraps the authoritative source through an application-owned monitor port. It
arms or polls the monitor before scanning, repeats only complete root-available observations, and
never repeats removals or diagnostics. Every authoritative scan invalidates the previous cache before
starting, including scans that fail. Three consecutive monitor failures disable notifications until
successful source reconfiguration. Monitor setup failure and panics use the same polling fallback.

The Windows monitor lives in the native crate's audited FFI boundary. It owns a single notification
handle, uses zero-timeout waits, rearms only a signaled handle, and closes through RAII. Unicode and
extended-length paths remain UTF-16. Root availability is checked separately because Win32
notifications describe children rather than the root itself. API ownership and rearm requirements
were checked on 2026-10-16 against Microsoft's documentation for
[FindFirstChangeNotificationW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw),
[FindNextChangeNotification](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findnextchangenotification),
[WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject),
and [FindCloseChangeNotification](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findclosechangenotification).

`SessionLogSelection` fixes Show New at the session start and computes the rolling display cutoff
from the current system time. Discovery refreshes at most once per monotonic minute in Last 24 Hours
mode. Mode changes propose an immediate source cutoff. The owner commits a proposed selection only
after source reconfiguration succeeds, so failures preserve the old selection and retry opportunity.

## Consequences

The portable pipeline can be exercised without Nexus, network requests, or credentials. Synthetic
tests cover stability changes, FIFO history, backpressure, failed admission, seeded rescans,
reconfiguration, cancellation, and panic containment. Filesystem tests cover Unicode, missing-root
recovery, exact cutoff boundaries, candidate limits, and Windows junction exclusion. Unix symlink
coverage remains in the portable suite for Linux CI.

Native tests exercise Unicode nested creation, modification, rename, deletion, missing-root recovery,
cancellation, and atomic invalid configuration. Notification cache/fallback and session-window timing
have deterministic portable tests. Wine validation remains required.

The development DLL remains a lifecycle shell. Runtime composition must schedule the pump and
selection updates, and persistent history must seed discovery before scanning. The shipping C++
implementation is unchanged.
