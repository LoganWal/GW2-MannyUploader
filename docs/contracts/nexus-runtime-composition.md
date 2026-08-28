# Nexus runtime composition contract

## Ownership

The production Windows runtime is created only after Nexus has supplied valid game/addon paths and a
compatible ImGui context. One composition root owns, in dependency order:

- the system clock, static-libcurl/Schannel transport, settings and upload-history stores, and
  protected-storage capability;
- provider clients, Twitch session owner, upload/authentication/verification/test workers, and the
  single-request DonBot aggregate worker;
- DonBot, Twitch, options, and recent-log action controllers plus the Win32 external-action adapter;
- EVTC reader/parser worker, log candidate source, upload/ingestion coordinators, and application
  pump; and
- one joined background application-owner thread.

Construction is transactional. Any failure destroys already-created components in reverse order and
joins every worker before returning a safe generic initialization error. DonBot's provider worker may
exist in a dormant no-guild state so a fresh default configuration can load; it rejects dispatch
until a verified guild has been persisted.

If native protected storage is unavailable, dps.report continues anonymously while DonBot and Twitch
credential operations expose the unavailable capability. Twitch connection additionally requires the
public Client ID saved through Nexus options or provided as the optional packager fallback. Missing
Twitch configuration never prevents dps.report, GW2Wingman, or DonBot from running.

## Application owner

The background owner is the only caller of options/action ticks, external target launching,
filesystem polling, discovery, ingestion, job mutation, retry dispatch, provider-configuration
updates, and upload-history persistence. It applies the latest durable dps.report, DonBot, and Twitch
settings before accepting new logs and publishes deep-copy, UI-ready snapshots at a bounded cadence.
UI command submission wakes the owner but performs no application work itself.

The dps.report provider update contains the Detailed WvW choice. Each accepted request captures the
current value, so a later toggle affects new uploads without changing queued work.

DonBot provider updates include a captured Discord delivery mode and optional channel ID. The
options snapshot supplies only DonBot-authorized transient channel choices. Native Windows and Wine
render the same server, delivery toggle, route selector, and enabled message-kind status.
The provider starts with delivery disabled. When persisted delivery is enabled, initial discovery
waits for saved verification, and only a current matching verified snapshot may activate its route.
Verification failure leaves delivery disabled and releases discovery without using stale routing.
While DonBot is enabled and verified, the application owner refreshes server authorization and
delivery properties every 5 minutes. Enabling DonBot starts the refresh immediately. Refresh work
runs through the existing verification worker, preserves the last verified snapshot while in flight,
and never performs HTTP or protected-storage I/O in a render callback.

The application owner also advances `DonBotAggregateDeliveryController`. The render callback queues
only selected stable job IDs and displayed revisions. The controller re-resolves jobs, guild
provenance, capability, bound, and route before it sends fight IDs to the dedicated worker. Worker
results become session-only immutable snapshots and never alter upload history.

Poll interval changes apply immediately. Log-directory, recursion, stability, history, candidate,
parser-capacity, and per-provider parallelism changes also apply live on this owner thread. They do
not replace the component graph:

- log-directory, recursion, and candidate-limit updates validate a replacement source configuration
  before committing it, then clear source-retained paths and pending stability observations;
- stability-threshold updates clear pending observations so observations collected under different
  thresholds cannot be combined;
- parser-queue downsizing preserves every active, queued, and completed parse, temporarily rejecting
  new requests until the queue is below its new bound;
- provider parallelism changes each provider's independent admission limit without cancelling active
  requests; and
- recent-history downsizing removes oldest settled jobs only. Active jobs may temporarily place the
  retained history above the new bound, new jobs wait for settled capacity, and excess rows are
  removed as active jobs settle.

Accepted-log dedupe entries, active upload jobs, captured provider configuration, and retry schedules
survive all general-settings changes. Persisted identities seed dedupe before the first poll, while
restored rows never dispatch automatically. Source, stability, and interval changes schedule an
immediate poll so the new behavior does not wait for the previous interval.

## Rendering

The main callback copies one published snapshot and renders the bounded recent-log table, including
detection time, per-provider state, aggregate selection checkboxes, aggregate-copy controls, and
typed folder/retry/reupload/rechat actions. Beside those aggregate controls it exposes the explicit
`Send selected logs via DonBot aggregate` command plus live dps.report, Detailed WvW, GW2Wingman,
DonBot, and DonBot Discord delivery toggles plus verified DonBot server and authorized Discord route
dropdowns. Each control submits a narrow value command. The options callback edits adapter-owned
bounded buffers and submits only value commands. Poll interval, stability observations, recent-log
limit, parser queue capacity, and candidate limit remain validated persisted settings but are not
exposed in Nexus. The Nexus page exposes Detailed WvW and per-provider parallelism, which defaults to
five.
Candidate DonBot keys
are converted directly into move-only secret commands and the input buffer is wiped immediately.
Neither callback traverses the filesystem, saves settings, accesses protected records, parses EVTC,
performs HTTP/OAuth work, advances jobs, or joins threads.

The quick-access shortcut and the Nexus-configurable input bind target the same callback-safe window
toggle. The default chord is `Alt+Shift+M`; Nexus owns rebinding. A press updates an atomic render
visibility flag immediately and queues a narrow durable visibility command, while a release does
nothing. Three small embedded PNG variants are decoded synchronously by Nexus during load. An owned
multiline tooltip lists enabled destinations, the selected DonBot guild, and its active Discord
delivery route. Grey means none enabled, the normal tint means upload enabled, and Twitch purple
takes precedence when chat is enabled.

## Shutdown

After the outer lifecycle has removed the shortcut/bind, closed/deregistered rendering, and drained
all admitted callbacks, the
runtime requests its owner thread to stop and wakes it. The owner stops command acceptance, cancels
test delivery and the application pump, persists final normalized upload history, shuts down
DonBot/Twitch/session/configuration owners, and returns. Runtime destruction then joins every worker
in reverse dependency order. No detached work or borrowed Nexus API pointer survives unload.

## Verification

Portable lifecycle tests cover partial registration, exception containment, idempotence, and unload
waiting on a blocked callback. The Windows smoke host loads the actual DLL through `GetAddonDef`,
provides a compatible real ImGui context and minimal Nexus API, renders main/options, invokes the
window bind, validates exact shortcut identifiers and all embedded PNG handoffs, verifies complete reverse
resource teardown, unloads the composed runtime, and calls `FreeLibrary` across ten consecutive
hot-load cycles.
