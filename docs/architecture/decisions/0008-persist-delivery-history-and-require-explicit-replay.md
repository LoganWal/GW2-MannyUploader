# ADR 0008: Persist delivery history and require explicit replay

- Status: Accepted
- Date: 2026-08-22

## Context

Provider enablement is live configuration, but treating an enable transition or addon restart as a
request to revisit old files can duplicate uploads and, more seriously, spam broadcaster chat.
Process-local file deduplication and Twitch's ambiguity ledger cannot protect a later game session.

## Decision

Persist a bounded ordinary history keyed by exact canonical path, size, and last-write time. Restore
recent job state for display, seed discovery with every retained identity, and never dispatch work as
part of restore. Normalize interrupted non-terminal states to visible failures.

Provider enablement applies only when a new stable identity creates a job. Replaying settled work is
available only through two explicit row actions: Reupload for dps.report/GW2Wingman/DonBot and Rechat
for Twitch. Both requests are marked user initiated so provider-local duplicate guards can distinguish
deliberate replay from automatic retry.

The history contains public results and normalized receipts but no credentials or retry deadlines. It
uses bounded JSON and atomic replacement in the Nexus addon data directory.

## Consequences

- Disabling/re-enabling providers, switching New/Last 24 Hours, and restarting the game do not replay a
  retained unchanged log.
- A changed file identity remains eligible after normal stability checks.
- Users retain a deliberate way to repeat uploads or chat without weakening automatic suppression.
- Corrupt history cannot be trusted and is visibly ignored; the New cutoff remains the startup safety
  boundary until valid history is written again.
- The complete data and restore rules are frozen in
  [`upload-history.md`](../../contracts/upload-history.md).
