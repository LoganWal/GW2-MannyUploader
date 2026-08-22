# Log polling and application-pump contract

This contract defines the first filesystem candidate source and the single-owner application tick
that connects it to stability, deduplication, metadata parsing, and upload-job creation. Polling is the
portable baseline and future fallback for native Windows directory notifications.

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

The standard polling source has a non-empty root, a recursive-traversal option, and a non-zero maximum
candidate count. A poll:

1. checks cancellation;
2. treats a missing root as an available outcome with `root_available=false`;
3. rejects a configured root that exists but is not a directory;
4. enumerates regular, non-symlink files;
5. filters `.zevtc` with ASCII case-insensitive matching;
6. resolves accepted paths with `std::filesystem::weakly_canonical`;
7. reads size and last-write time without throwing; and
8. sorts observations, seen paths, issues, and removals for deterministic consumers.

Directory symlinks and file symlinks are not followed. This keeps candidates within the configured
tree and avoids traversal cycles.

The candidate limit is checked before a snapshot becomes visible. Exceeding it returns
`ResourceLimit` and preserves the last successfully reconciled state.

The application owner may replace the root, recursion option, and candidate limit in place. The
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

An entry whose path is known but whose size or timestamp cannot be read is included in `seen_paths`
but not in `observations`.

## Application tick

An application tick receives the currently enabled provider selection and performs bounded work:

1. Consume at most `max_metadata_results_per_tick` completed parser results and correlate them by
   `UploadJobId` through `LogIngestionCoordinator`.
2. Poll the candidate source.
3. Forget removed paths from the stability tracker.
4. Pass every observation through stability and exact-identity deduplication.
5. Submit each newly stable identity for asynchronous metadata parsing.

If job submission fails before the file is accepted, its exact dedupe key is released so a later
observation can retry. A parser queue rejection is different: `LogIngestionCoordinator` retains a
failed job, so the file remains accepted and deduplicated.

The tick report contains counts, root/scan status, and source issues. Hard candidate-source,
discovery, ingestion, and shutdown failures remain typed.

The owner may also replace the stability threshold while the pump is live. A valid replacement clears
pending stability observations without clearing dedupe. A threshold below two is rejected without
changing either the current threshold or pending state.

## Shutdown

`ApplicationPump::cancel_all()` is idempotent. It stops later ticks and delegates cancellation to
`LogIngestionCoordinator`, which stops the parser and providers. The synchronous polling source owns
no thread; an in-progress scan observes the caller's stop token between entries.

Future native watcher events must implement the same `ILogCandidateSource` batch semantics. Repeated
native failures may switch composition to this polling source without changing discovery or job
coordination.
