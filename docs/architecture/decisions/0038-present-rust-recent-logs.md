# ADR 0038: Present Rust recent logs from immutable snapshots

- Status: Accepted
- Date: 2026-10-16

## Decision

Keep recent-log and quick-access presentation in the portable UI layer. The application supplies
coordinator snapshots, current destination selection, and the session's display cutoff. Pure model
builders perform no I/O, dispatch, mutation, or persistence. The native renderer will consume their
values and submit existing typed commands.

Recent rows reverse coordinator order and filter by file modification time. All retained job IDs
remain available to selection reconciliation, including hidden jobs. Changing destination visibility
does not alter retained provider states. Input count cannot exceed the coordinator's history bound,
and duplicate job IDs are rejected.

Rows contain encounter labels, UTC detection time, normalized provider cells, retained DonBot fight
IDs, validated report links, and action availability. They omit the raw log filename and full local
path. Open-directory availability is derived from the retained parent without filesystem access.
Retry requires a failed provider and parsed metadata. Reupload requires idle upload providers and
metadata. Rechat requires a validated retained report and idle Twitch. Executing an action still
revalidates the current job through the application controller.

Report links share the action controller's trusted-origin checks. Visible dps.report links are
joined in row order with newlines. A visible-row DonBot aggregate URL is constructed from bounded
numeric receipt IDs. The separate selected aggregate model supplies a URL and stable job IDs for
selected eligible visible rows. Selection intent survives temporary destination or route loss.
Sending requires current verified authorization, an authorized route, unique fight IDs, at least two
fights, and the lower of the server limit and 100. Refresh, queued/sending delivery, and shutdown
disable sending while preserving selection and copied-link data.

UTC formatting uses Gregorian cycles without operating-system locale or timezone dependencies.
Subsecond times before the Unix epoch floor toward the earlier second. Output supports years 1
through 9999 and otherwise uses a fixed unavailable label. Calendar tests exercise pre-1601 values
directly because Windows SystemTime cannot represent them. Native timestamp tests use its supported
range and 100-nanosecond resolution.

Quick-access text starts with `View MannyUploader list`, then lists active destinations and DonBot
summary routing. Verified guild/channel names fall back to the selected guild ID or a generic route
label. Control characters in remote names become spaces so names cannot introduce extra status lines.
Twitch tint takes priority over upload tint, and no active destination uses idle tint. Native icon
colors and resource updates remain adapter responsibilities.

## Validation and remaining integration

Portable tests cover filtering and retention, copied-link order, live destination visibility,
hostile links, failure-health labels, provider-state action matrices, UTC boundaries, aggregate
selection and limits, Unicode names, and every destination combination.

The development DLL still needs full application composition, native list/options rendering,
clipboard operations, and reactive quick-access registration. No test opens a real report, changes
the system clipboard, or sends an aggregate. Existing release and Windows/Wine parity gates remain.
