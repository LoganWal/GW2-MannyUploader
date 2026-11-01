# ADR 0036: Run recent-log actions outside rendering

- Status: Accepted
- Date: 2026-10-16

## Decision

RecentLogActionsController owns a preallocated, bounded, thread-safe FIFO. Render callbacks submit
typed commands containing stable job IDs and known report/provider variants. The application owner
borrows its upload coordinator and external launcher while draining a bounded initial batch. A
reentrant tick is rejected, and commands submitted during external work wait for a later tick.
No queue lock crosses a coordinator or launcher call.

Actions resolve their target from the current retained job. The coordinator now provides one-job
snapshot lookup so opening a report does not clone the entire history. Trusted dps.report, WvW,
and Wingman origins require nonempty suffixes, bounded printable ASCII, and no backslash or quote.
The WvW origin also requires retained report boss ID 1. DonBot links are constructed from positive
signed-64-bit fight IDs. Directory actions use the exact nonempty parent of the retained path.
No path or URL appears in action errors or queue snapshots.

Retry, Reupload, and Rechat delegate to the coordinator's existing state and dependency rules.
Dispatch failures remain failed jobs and visible action errors. Successful actions clear the last
error, and a dedicated dismiss command clears it without another external action. Every published
queue count change advances the snapshot revision, including removing a command before launch.
Shutdown clears queued commands and prevents late completions from overwriting shutdown state.
The runtime must drain an already admitted application tick before destroying its dependencies.

WindowsExternalActionLauncher implements the application-owned port in the native crate. It uses
bounded, terminated UTF-16 and ShellExecuteW with the open verb and no command-line parameters.
Directory conversion preserves Windows path encoding. Return values become typed generic errors.
The adapter does not use a command interpreter and does not expose OS target text in diagnostics.

DonBotAggregateSelection is independent UI state keyed by endpoint, account, and guild. Newly
eligible visible rows are selected once. Positive selection and explicit deselection survive
temporary ineligibility and hidden rows. Eviction prunes retained state, identity changes reset it,
and selected-visible output follows display order without duplicate job IDs. Delivery availability
remains a separate application-controller decision.

## Verification and remaining work

Seventeen portable tests cover exact retained targets, hostile links, WvW origin gating, invalid
fight IDs, history eviction, missing directories/receipts, concurrent submission, bounded draining,
reentrant callbacks, launch errors/panics, shutdown during launch, provider retry failure, distinct
replay intents, and selection retention/reset. A regression observed failing before its fix checks
snapshot revision changes before external work.

Two native adapter tests cover UTF-16 preservation, termination, length/NUL limits, and ShellExecute
status mapping through an injected call boundary. No test launches a real browser or directory.
Interactive native-Windows and Wine opening still require runtime integration and manual checks.

Recent-row and options presentation, copied links, the options command controller, production
runtime composition, active-work DLL unload, packaging, symbols, and hosted release gates remain
pending. The native development DLL still owns only its lifecycle shell.
