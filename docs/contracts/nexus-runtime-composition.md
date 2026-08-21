# Nexus runtime composition contract

## Ownership

The production Windows runtime is created only after Nexus has supplied valid game/addon paths and a
compatible ImGui context. One composition root owns, in dependency order:

- the system clock, static-libcurl/Schannel transport, settings store, and protected-storage
  capability;
- provider clients, Twitch session owner, upload/authentication/verification/test workers;
- DonBot, Twitch, and options controllers;
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

The background owner is the only caller of options ticks, filesystem polling, discovery, ingestion,
job mutation, retry dispatch, and provider-configuration updates. It applies the latest durable
DonBot/Twitch settings before accepting new logs and publishes deep-copy, UI-ready snapshots at a
bounded cadence. UI command submission wakes the owner but performs no application work itself.

Poll interval changes apply immediately. Log-directory, recursion, stability, history, candidate,
and parser-capacity changes require addon reload in the initial adapter because those values shape
long-lived bounded components; the options UI states this before save.

## Rendering

The main callback copies one published snapshot and renders the bounded recent-log table. The options
callback edits adapter-owned bounded buffers and submits only value commands. Candidate DonBot keys
are converted directly into move-only secret commands and the input buffer is wiped immediately.
Neither callback traverses the filesystem, saves settings, accesses protected records, parses EVTC,
performs HTTP/OAuth work, advances jobs, or joins threads.

## Shutdown

After the outer lifecycle has closed/deregistered the callbacks and drained admitted rendering, the
runtime requests its owner thread to stop and wakes it. The owner stops command acceptance, cancels
test delivery and the application pump, shuts down DonBot/Twitch/session/configuration owners, and
returns. Runtime destruction then joins every worker in reverse dependency order. No detached work
or borrowed Nexus API pointer survives unload.

## Verification

Portable lifecycle tests cover partial registration, exception containment, idempotence, and unload
waiting on a blocked callback. The Windows smoke host loads the actual DLL through `GetAddonDef`,
provides a compatible real ImGui context and minimal Nexus API, renders main/options once, verifies
reverse callback deregistration, unloads the composed runtime, and calls `FreeLibrary`.
