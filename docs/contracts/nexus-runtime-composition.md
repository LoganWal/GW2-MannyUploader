# Nexus runtime composition contract

## Ownership

The production Windows runtime is created only after Nexus has supplied valid game/addon paths and a
compatible ImGui context. One composition root owns, in dependency order:

- the system clock, static-libcurl/Schannel transport, settings store, and protected-storage
  capability;
- provider clients, Twitch session owner, upload/authentication/verification/test workers;
- DonBot, Twitch, options, and recent-log action controllers plus the Win32 external-action adapter;
- EVTC reader/parser worker, log candidate source, upload/ingestion coordinators, and application
  pump; and
- one joined background application-owner thread.

Construction is transactional. Any failure destroys already-created components in reverse order and
joins every worker before returning a safe generic initialization error. DonBot's provider worker may
exist in a dormant no-guild state so a fresh default configuration can load; it rejects dispatch
until a verified guild has been persisted.

If native protected storage is unavailable, dps.report continues anonymously while DonBot and Twitch
credential operations expose the unavailable capability. If no public Twitch application ID was
compiled into the addon, Twitch connection is explicitly disabled without preventing dps.report,
GW2Wingman, or DonBot from running.

## Application owner

The background owner is the only caller of options/action ticks, external target launching,
filesystem polling, discovery, ingestion, job mutation, retry dispatch, and provider-configuration
updates. It applies the latest durable DonBot/Twitch settings before accepting new logs and publishes
deep-copy, UI-ready snapshots at a bounded cadence. UI command submission wakes the owner but performs
no application work itself.

Poll interval changes apply immediately. Log-directory, recursion, stability, history, candidate,
and parser-capacity changes also apply live on this owner thread. They do not replace the component
graph:

- log-directory, recursion, and candidate-limit updates validate a replacement source configuration
  before committing it, then clear source-retained paths and pending stability observations;
- stability-threshold updates clear pending observations so observations collected under different
  thresholds cannot be combined;
- parser-queue downsizing preserves every active, queued, and completed parse, temporarily rejecting
  new requests until the queue is below its new bound; and
- recent-history downsizing removes oldest settled jobs only. Active jobs may temporarily place the
  retained history above the new bound, new jobs wait for settled capacity, and excess rows are
  removed as active jobs settle.

Accepted-log dedupe entries, active upload jobs, captured provider configuration, and retry schedules
survive all general-settings changes. Source, stability, and interval changes schedule an immediate
poll so the new behavior does not wait for the previous interval.

## Rendering

The main callback copies one published snapshot and renders the bounded recent-log table, including
detection time and typed report/folder/retry actions. The options callback edits adapter-owned bounded
buffers and submits only value commands. Candidate DonBot keys
are converted directly into move-only secret commands and the input buffer is wiped immediately.
Neither callback traverses the filesystem, saves settings, accesses protected records, parses EVTC,
performs HTTP/OAuth work, advances jobs, or joins threads.

The quick-access shortcut and the Nexus-configurable input bind target the same callback-safe window
toggle. The default chord is `Alt+Shift+M`; Nexus owns rebinding. A press updates an atomic render
visibility flag immediately and queues a narrow durable visibility command, while a release does
nothing. The shortcut icon is a small embedded PNG decoded synchronously by Nexus during load.

## Shutdown

After the outer lifecycle has removed the shortcut/bind, closed/deregistered rendering, and drained
all admitted callbacks, the
runtime requests its owner thread to stop and wakes it. The owner stops command acceptance, cancels
test delivery and the application pump, shuts down DonBot/Twitch/session/configuration owners, and
returns. Runtime destruction then joins every worker in reverse dependency order. No detached work
or borrowed Nexus API pointer survives unload.

## Verification

Portable lifecycle tests cover partial registration, exception containment, idempotence, and unload
waiting on a blocked callback. The Windows smoke host loads the actual DLL through `GetAddonDef`,
provides a compatible real ImGui context and minimal Nexus API, renders main/options, invokes the
window bind, validates exact shortcut identifiers and embedded PNG handoff, verifies complete reverse
resource teardown, unloads the composed runtime, and calls `FreeLibrary` across ten consecutive
hot-load cycles.
