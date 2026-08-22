# Log candidate sources and application-pump contract

This contract defines the filesystem candidate sources and the single-owner application tick that
connects them to stability, deduplication, metadata parsing, and upload-job creation. Polling is the
portable baseline and the authoritative scan behind native Windows directory notifications.

## Scheduling and ownership

Polling is synchronous from the caller's perspective. The eventual addon runtime schedules it on the
application coordinator thread; Nexus render and options callbacks must never call it directly.

One `ApplicationPump` owns the discovery policy and is the only object allowed to:

- reconcile candidate observations and removals;
- submit stable files to `LogIngestionCoordinator`; and
- consume completed metadata results from `ILogMetadataParser`.

Each tick drains a configured maximum number of parser results before scanning. This releases the
worker's bounded output backpressure before new parse requests can be submitted. Mutable upload jobs
remain owned by `UploadCoordinator`.

## Portable polling source

The standard polling source has a non-empty root, a recursive-traversal option, a non-zero maximum
candidate count, and an optional minimum last-write cutoff. A poll:

1. checks cancellation;
2. treats a missing root as an available outcome with `root_available=false`;
3. rejects a configured root that exists but is not a directory;
4. enumerates regular, non-symlink files;
5. filters `.zevtc` with ASCII case-insensitive matching;
6. reads last-write time and excludes files older than the active cutoff;
7. resolves accepted paths with `std::filesystem::weakly_canonical` and reads size without throwing;
8. sorts observations, seen paths, issues, and removals for deterministic consumers.

Directory symlinks and file symlinks are not followed. This keeps candidates within the configured
tree and avoids traversal cycles.

The candidate limit applies only after the cutoff filter. This permits a large historical archive
while bounding the active New/Today window. Exceeding the eligible limit returns `ResourceLimit` and
preserves the last successfully reconciled state.

The application owner may replace the root, recursion option, candidate limit, and cutoff in place. The
replacement is fully validated before mutation; failure preserves the old configuration and retained
snapshot. Success clears the retained snapshot and asks `ApplicationPump` to clear pending stability
observations, because old-root removals and new-root observations must not be reconciled together.
The pump deliberately preserves exact-identity deduplication, active metadata requests, and upload
jobs.

## Missing directories, incomplete scans, and removals

The source retains only paths from the latest authoritative snapshot:

- When the root is missing, every previously seen candidate is emitted as removed and retained state
  is cleared. A later poll discovers the directory normally if it appears.
- A complete scan emits paths that disappeared since the previous authoritative snapshot.
- If traversal or per-entry metadata fails, the batch is marked incomplete and includes a concise
  issue. No removals are emitted from that scan. Seen paths are merged into retained state so a
  transient access failure cannot reset file stability or manufacture a removal.
- A later complete scan reconciles and emits real removals.

An eligible entry whose canonical path or size cannot be read marks the scan incomplete. A timestamp
failure cannot establish cutoff eligibility and is likewise reported without an observation.

## Session log selection

Production starts in `Show New` with the addon-load file-clock time as its cutoff. `Show Today` uses
local calendar midnight and refreshes the cutoff if the local day changes. Both modes continue to
include later files. Switching modes reconfigures the source and clears pending stability samples but
preserves exact-identity deduplication, active jobs, and provider work. The recent-log view applies
the same cutoff, so logs outside the selected session window are hidden without cancelling work that
already started.

## Native Windows notification front end

Native Windows composition wraps the polling source with a nonblocking directory-change monitor. It
uses `FindFirstChangeNotificationW`, a zero-timeout `WaitForSingleObject`, and
`FindNextChangeNotification`; it owns one RAII handle and creates no thread. File-name,
directory-name, size, creation, and last-write changes are observed, with the configured recursive
policy passed to Windows.

The first poll and every changed or unavailable notification run the standard polling source as the
authoritative scan. An unchanged notification returns the last complete root-available observations
so the stability policy can advance without traversing the tree again. Cached batches never replay
removals or issues. Missing and incomplete roots are never cached, so polling continues until the root
appears or a complete scan succeeds.

Notification failures do not fail log ingestion when the authoritative poll succeeds. They produce a
bounded diagnostic and use polling for that cycle. Three consecutive notification failures disable
the monitor and retain ordinary polling for the rest of that configuration. A successful settings
reconfiguration atomically replaces the polling source, clears its notification cache, and attempts
to enable the monitor for the new root again. Invalid settings preserve the complete prior source.

## Application tick

An application tick receives the currently enabled provider selection and performs bounded work:

1. Consume at most `max_metadata_results_per_tick` completed parser results and correlate them by
   `UploadJobId` through `LogIngestionCoordinator`.
2. Poll the candidate source.
3. Forget removed paths from the stability tracker.
4. While the parser has input capacity, pass observations through stability and exact-identity
   deduplication.
5. Submit each newly stable identity for asynchronous metadata parsing. Remaining observations are
   left untouched and reconsidered on the next poll rather than converted into failed jobs.

If job submission fails before the file is accepted, its exact dedupe key is released so a later
observation can retry. The capacity check prevents ordinary parser saturation from reaching the
coordinator. A parser rejection after that check is an unexpected dispatch failure;
`LogIngestionCoordinator` retains a visible failed job, so the file remains accepted and
deduplicated.

The tick report contains counts, root/scan status, and source issues. Hard candidate-source,
discovery, ingestion, and shutdown failures remain typed.

The owner may also replace the stability threshold while the pump is live. A valid replacement clears
pending stability observations without clearing dedupe. A threshold below two is rejected without
changing either the current threshold or pending state.

## Shutdown

`ApplicationPump::cancel_all()` is idempotent. It stops later ticks and delegates cancellation to
`LogIngestionCoordinator`, which stops the parser and providers. Neither candidate source owns a
thread. An in-progress scan observes the caller's stop token between entries, and the native monitor
checks it before querying its handle. Destruction closes the native handle; repeated native failures
switch to polling without changing discovery or job coordination.
