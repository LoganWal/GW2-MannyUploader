# Recent-log action contract

## Boundary

The main Nexus render callback reads an immutable recent-log snapshot and may submit only typed
commands. It never launches a process, traverses the filesystem, mutates an upload job, or dispatches
a provider directly. `RecentLogActionsController` owns a bounded thread-safe FIFO; the background
application owner drains at most a configured number of commands per tick.

Supported commands are:

- open the retained job's dps.report permalink;
- open the retained job's GW2Wingman permalink;
- open the retained job's exact DonBot fight page;
- open the retained log's containing directory;
- retry one failed provider for the retained job;
- explicitly reupload the retained log to dps.report, GW2Wingman, and DonBot;
- explicitly post the retained dps.report result to Twitch chat again; and
- dismiss the last action error.

Commands carry a stable non-zero `UploadJobId`, and retry also carries a known provider ID. A stale
job, full queue, invalid command, failed launch, invalid state, or shutdown produces a local generic
error in a secret-free snapshot. Submission itself performs no external action.

## External targets

The controller resolves the target from the upload coordinator's current snapshot rather than
accepting a URL or path from ImGui. A dps.report or GW2Wingman target must:

- begin with its exact trusted origin and contain a non-empty suffix;
- be at most 2,048 bytes;
- contain printable ASCII only; and
- contain no backslash or double quote.

The trusted origins are `https://dps.report/` and
`https://gw2wingman.nevermindcreations.de/log/`. A DonBot target is constructed locally as
`https://donbot.walmslo.com/logs/{fight_log_id}` from the retained numeric receipt. The folder target
is exactly the parent of the retained canonical log path and must be non-empty.
The Windows adapter passes either validated target directly to `ShellExecuteW`; it does not construct
a command line or invoke a command interpreter. Launching occurs on the application-owner thread.

## Manual retry

Only a provider in `Failed` state with already-parsed encounter metadata may be retried manually.
Succeeded, skipped, disabled, cancelled, waiting, active, and scheduled-retry states cannot be
re-armed. A metadata-parse failure is recovered by detecting the log again because no upload request
can be reconstructed safely.

A manual retry transitions the provider through `Waiting` to `Active`, increments its attempt count,
and marks the queued request as user initiated. If a failed dps.report attempt had skipped its waiting
Twitch dependency, re-arming dps.report also returns that dependency to `Waiting`; a later successful
report dispatches Twitch normally.

Twitch continues to suppress every automatic resend after ambiguous delivery. A user-initiated retry
may bypass an ambiguous ledger entry. A normalized failed drop receipt is cleared while the failed
Twitch state is re-armed so the new attempt can record its own receipt. A confirmed sent job is
`Succeeded`, so it cannot expose or execute the retry command.

If enqueueing the retry fails, the provider returns to `Failed` with the dispatch diagnostic and the
controller reports the action failure. No command reports success while leaving an unqueued active
attempt.

## Explicit replay

`Reupload` is distinct from failed-provider retry. It requires parsed encounter metadata and requires
all three upload destinations to be idle. It atomically resets the dps.report result, GW2Wingman
permalink, and DonBot receipt, then dispatches dps.report, GW2Wingman, and DonBot with the
user-initiated flag regardless of their state when the log was first detected. Twitch is not part of
this action.

`Rechat` requires a retained dps.report result and an idle Twitch state. It clears the prior Twitch
receipt and queues one user-initiated chat attempt. That flag deliberately bypasses both confirmed
and ambiguous entries in the process-local Twitch ledger. These are the only controls that replay a
settled log; enabling a provider, switching New/Today mode, or restarting the game never does so.

## Shutdown and verification

Shutdown clears queued commands, rejects later submissions, and is idempotent. The controller owns no
thread and borrows the coordinator and launcher for its complete lifetime.

Deterministic tests cover trusted and hostile report links, derived folders, stale jobs, queue bounds,
launch failures, state rejection, provider re-dispatch, dps.report/Twitch dependency recovery,
explicit ambiguous Twitch resend, receipt replacement, and shutdown. No test opens a real browser,
folder, or public service.
