# GW2 Manny Uploader

GW2 Manny Uploader is a clean-slate, streamlined arcdps log uploader for the
[Raidcore Nexus](https://raidcore.gg/Nexus) addon framework.

The project is currently in active implementation and hardening. The repository builds a Nexus DLL
and a testable core library with an upload-job state machine, provider ports,
immutable snapshots, a single-owner upload coordinator, and deterministic `.zevtc` stability/dedupe
policy. Stable files enter an asynchronous, job-ID-correlated metadata-parser boundary before any
provider dispatch. A bounded worker extracts exactly one EVTC entry from each ZIP with cancellation,
resource ceilings, and CRC validation, then a bounds-checked decoder extracts boss ID and POV account
metadata. Versioned ordinary settings have pure validation, strict JSON parsing, crash-safe atomic
saves, and last-known-good recovery.

Credentials are separate move-only values behind an application port. Native Windows persistence uses
user-scoped, prompt-free DPAPI over bounded, versioned records with atomic replacement and no secret
backups. The adapter deliberately fails closed under Wine because Wine DPAPI does not provide the same
credential-binding guarantees; secure Wine persistence remains unavailable until a reviewed host
keyring bridge exists. An application-owned configuration service now loads ordinary settings,
reports that protected-storage capability without exposing credential state, applies durable
write-through updates, and rejects persistence after shutdown.

A provider-independent HTTP port and its Windows production adapter are implemented. The adapter
uses pinned static libcurl 8.21.0 with Schannel, streams exact-length request bodies, bounds responses,
supports cooperative cancellation, refuses automatic redirects, and keeps sensitive values out of
typed errors. Deterministic loopback tests run under Wine; an opt-in HTTPS probe verifies the Schannel
path without making public network access part of the default suite.

The dps.report, direct GW2Wingman, DonBot, and synchronous Twitch provider clients are now
implemented. dps.report streams the stable `.zevtc` and optional protected user token, validates
trusted report metadata, and returns token rotation separately for persistence before success.
GW2Wingman streams the stable file with its POV account and encounter ID through an explicitly
isolated raw-EVTC compatibility bridge; this avoids bundling Elite Insights and .NET while
acknowledging that the bridge is not part of Wingman's current public API. DonBot verifies the
protected GW2 API key, accepts only server-authorized guilds, and streams through a strict same-origin
TUS handshake with `wingman=false`. Twitch implements public-client Device Code OAuth, validation,
rotating-token refresh, revocation, and same-broadcaster Helix chat delivery with no client secret or
configurable destination. All clients use strict bounded responses and generic retry/error details.

All three upload-provider wrappers own the same reusable asynchronous worker: one joined thread,
bounded FIFO input/output queues, result backpressure, exception containment, and cooperative
cancellation. The application pump drains results with a per-tick bound and keeps retry deadlines
under the single-owner coordinator. DonBot freezes ordinary endpoint/guild configuration at enqueue
and loads its protected key only when the attempt starts. A separate application-owned controller now
verifies candidate or saved keys asynchronously, retains account/guild identity only in secret-free
transient snapshots, and applies fail-safe settings/key/selection/disconnect persistence ordering.

Twitch authentication now has its own primitive worker and application-owned controller. Device Code
polling, startup/hourly validation, pre-expiry rotation, one reconnect recovery, versioned
whole-session protection, store-before-connected ordering, and disable/revoke/erase disconnect
behavior are covered by fake-clock and fake-persistence tests. Snapshots expose login and
authorization instructions but no token, device code, user ID, or protected-record data.
Twitch chat delivery compiles the version-1 six-field template, validates the fully rendered
500-code-point message, captures result policy per queued job, and returns typed sent/drop receipts.
Its provider worker obtains only a broadcaster-owned delivery lease, performs at most one controlled
recovery after `401`, retries only failures known not to have posted, and suppresses duplicates after
confirmed or ambiguous delivery. An explicit retry from the recent-log UI is separately marked and
may resend an ambiguous or rejected failed delivery; automatic work still cannot bypass that guard.
The same delivery policy now backs an isolated options test-message
worker, which posts fixed request-ID-suffixed text to the connected broadcaster's own chat without
inventing an upload job or dps.report result and never retries automatically after ambiguity.
Deterministic fake-transport and worker suites perform no public upload. A bounded Nexus-options
controller now exposes only secret-free snapshots and move-owned
commands: render-side submission performs no persistence or provider work, while the application
owner drains settings, DonBot, and broadcaster-owned Twitch actions outside rendering. Correlated test
delivery state and control enablement are part of the same composite snapshot. A pure UI model
supplies tested status labels and control enablement without depending on ImGui.

A portable polling source now handles missing or newly created log directories, recursive Unicode
paths, bounded candidate scans, and authoritative removals. A bounded application pump connects those
observations to stability, deduplication, parser-result drainage, and upload-job creation. Saved
general options are applied live on the application-owner thread: poll source, stability threshold,
parser queue, and recent-history bounds change without replacing the worker graph or interrupting
active parses/uploads. Source or stability changes discard only pending observations; accepted-log
deduplication remains intact.

The Windows DLL now exports the official Nexus API-v6 `GetAddonDef`, installs Nexus's ImGui 1.80
context and allocators, and composes the complete application behind one background owner thread.
Render callbacks consume immutable UI-ready snapshots and submit bounded commands only. The main
window shows detection time, recent logs, per-provider state/retry controls, and validated open-report
and open-folder actions; Nexus options expose ordinary settings, DonBot verification/guild selection,
and same-broadcaster Twitch connection, policy, and test-message controls. A callback gate contains
exceptions, reverses partial registration, waits for admitted callbacks, joins every runtime worker,
and supports deterministic hot-unload. The main window can be toggled from a quick-access upload icon
or the Nexus-managed `Alt+Shift+M` default keybind; users can rebind it through Nexus. A Windows smoke
host loads the real DLL, validates rendering, bind and shortcut registration, reverse teardown, and
completes ten consecutive unload and `FreeLibrary` cycles.

## Planned integrations

- [dps.report](https://dps.report/)
- GW2Wingman
- DonBot
- The broadcaster's own Twitch chat, using Twitch OAuth and the Helix Send Chat Message API

Twitch posting will depend on a successful dps.report upload and will publish a configurable message
containing the resulting permalink.

## Design goals

- A small, native Windows x64 Nexus addon
- Modern C++ with deterministic ownership and clean hot-unloading
- No parsing, network requests, or settings writes on the render thread
- Minimal EVTC parsing limited to metadata required by upload providers
- Independently testable provider clients and state transitions
- First-class Windows and Wine validation
- Secure handling of API keys and OAuth refresh tokens

## Requirements

- CMake 3.25 or newer
- Ninja for the included local presets
- A compiler with C++23 support
  - Visual Studio 2022/MSVC for the production Windows x64 DLL
  - A current GCC or Clang for development-only Linux builds

The ZIP, JSON, Windows HTTP, Nexus API, and Dear ImGui dependencies are pinned and hash-verified
through CMake. Nexus API v6 and ImGui 1.80 match the exact commits in Raidcore's current official C++
template; the announced future ImGui 1.92.7 host ABI is intentionally not mixed into this build. A
fresh dependency cache therefore requires network access during CMake configuration.

Twitch is a public OAuth client and never uses a client secret. Production builds inject the
registered public application ID at configure time:

```sh
cmake -S . -B out/build/release -DMANNY_TWITCH_CLIENT_ID=yourlowercaseclientid
```

Without that value, the addon remains loadable and all non-Twitch providers work, but Twitch account
connection is explicitly disabled in Nexus options.

## Build and test

Development build:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Optimized build:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The Windows plugin artifact is written beneath the selected build directory's `bin` directory as
`manny_uploader.dll`. Development-only non-Windows builds retain a bootstrap module so the portable
core remains easy to compile and test without Windows or Nexus.

## Repository layout

```text
cmake/                       CMake helpers
docs/architecture/           Architecture and decision records
docs/contracts/              Independently written provider contracts
include/manny_uploader/      Public core headers
src/                         Plugin and core implementation
tests/                       Unit and smoke tests
.github/workflows/           Continuous integration
```

Runtime-provider code will be organized into addon, application, configuration, EVTC, filesystem,
provider, UI, platform, and support components as the project grows.

The detailed dependency rules and runtime ownership model are documented in
[`docs/architecture/overview.md`](docs/architecture/overview.md).

Pinned third-party licenses are reproduced in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Development policy

This is a clean-slate implementation. External API contracts, public ABI documentation, sample
`.zevtc` files, and observable behavior may be used as specifications, but implementation code from
the previous uploader is not copied or translated into this repository.

Local AI-assistant instructions, prompts, agent state, and generated context are intentionally ignored
by Git. They must not be committed to this repository.
