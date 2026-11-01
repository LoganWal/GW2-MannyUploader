# ADR 0050: Publish validated Nexus updates

- Status: Accepted
- Date: 2026-11-01

## Decision

Use `year.month.day.GitHub-run-number` for Nexus addon versions. The previous day-before-month
ordering went backward at month boundaries. Both build workflows use one UTC version generator and
test the ordering across month and year boundaries. Local builds use a fixed development fallback.

Keep release publication separate from the manual native validation workflow. After the hosted
checks, native in-game matrix, and external release gates pass, manually dispatch `Publish Nexus
release` from the validated commit. The job requires the successful validation run ID, verifies
that its source commit matches, checks the ZIP checksum and layout, and publishes the exact verified
DLL as a standalone GitHub Release asset alongside the ZIP and checksum. The standalone DLL is the
Nexus update payload. The ordinary MinGW build remains a development artifact.

Changing the component order makes the first correctly ordered version compare below some local
October development DLLs. Those local copies need a one-time manual replacement. Subsequent release
versions increase by date.
