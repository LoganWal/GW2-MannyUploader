# ADR 0037: Coordinate Rust options outside rendering

- Status: Accepted
- Date: 2026-10-16

## Decision

Implement the frozen Nexus options contract with an application-owned controller and a separate
cloneable render handle. The controller owns the DonBot, Twitch authentication, test-message, and
external-action dependencies. The handle exposes only copied snapshots, bounded command submission,
and admission closure. No storage, workflow, or worker reference crosses into rendering.

Submission validates bounded values in memory and admits at most 32 commands by default. Capacity
must be between 1 and 256, and the per-tick limit must fit capacity. An application tick executes
at most its initial budget, eight by default, then polls each workflow once. Commands admitted
during an external action wait until another tick. Queue locks never cross persistence, worker
dispatch, or browser launching. Popping a command advances the public revision before external work.

Ordinary drafts merge against current settings when executed. Destination flags and verified DonBot
routing are absent from drafts. Visibility and destination toggles use dedicated commands. The Rust
ordinary merge preserves execution-time window visibility even though GeneralSettings carries it.
Copying visibility from the renderer's last snapshot is insufficient when an earlier queued toggle
has not yet published. A regression covers both visibility directions within and across tick budgets.
This tightens the existing C++ merge without changing the persisted settings format. Disabling
dps.report disables Twitch posting while retaining authentication. Client ID changes require a
disconnected or error state. ConfigurationService still validates and saves before publishing.

DonBot mutations delegate to its verified configuration workflow. Enabling requires a matching
endpoint and authorized selected guild, saves first, and requests a saved-key refresh. Refreshing
disables mutation controls and rejects upload-toggle commands. Disabling clears delivery routing.
Twitch test commands delegate to the correlated single-flight test controller without automatic
retry. Authorization commands carry only the displayed connection revision. Execution resolves a
current bounded Twitch activation URI and catches launcher failures with a fixed safe diagnostic.

Twitch operations execute without holding ConfigurationService's mutex because their shared session
owner acquires that mutex itself. A storage-adapter panic leaves the configuration mutex poisoned.
Snapshot publication retains the previous settings and reports a safe internal error. Subsequent
configuration mutations fail closed. A deterministic test first reproduced lost error publication
and now proves containment and retained settings.

The pure view model maps public snapshots to labels and control availability. It exposes a fixed
eight-asterisk authorization mask and no activation URI. Diagnostics are static text derived from
typed errors. Wine compatibility storage is explicitly described as reduced protection. DonBot
summary and aggregate route visibility remain independent. ImGui label escaping breaks every
adjacent hash pair while preserving Unicode text.

Admission closure immediately destroys queued commands, including wiping owned secret values.
The application owner then calls shutdown, which cancels the test-message controller exactly once
and publishes shutdown. A tick after admission closure performs that shutdown without dispatch.
Shutdown does not revoke authentication or modify persistence. Owned workflow destructors handle
their existing cancellation and joined worker destruction. Runtime composition must close callback
admission and drain callbacks before destroying the owner.

## Validation and remaining integration

Portable tests cover queue bounds, concurrent admission, FIFO budgets, no submission I/O,
write-through failure, stale drafts, narrow visibility, DonBot verification and refresh, Twitch
dependencies, authorization revision checks, launcher reentrancy and panics, test-message ambiguity,
shutdown, safe diagnostics, and view-model state matrices.

The native DLL remains a lifecycle shell. Runtime composition still needs startup connection
restoration, periodic DonBot refresh, packaged public Client ID selection, and applying committed
settings to provider clients and workers before subsequent dependent commands. Native ImGui options
rendering and the complete Linux/Wine and hosted release gates remain separate work.
