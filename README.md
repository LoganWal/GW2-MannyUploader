# GW2 Manny Uploader

GW2 Manny Uploader is a native Windows x64 [Raidcore Nexus](https://raidcore.gg/gw2/nexus) addon
that watches completed arcdps `.zevtc` logs and sends them to the services you enable without
blocking the game thread.

## Supported destinations

- [dps.report](https://dps.report/)
- Direct GW2Wingman
- DonBot
- The authenticated broadcaster's own Twitch chat

Each upload destination runs independently. Twitch waits for the same log's successful dps.report
result, then posts a configurable message containing that trusted permalink. Twitch has no channel
setting: the authenticated account is always both sender and broadcaster.

## Requirements

- Guild Wars 2 on native Windows x64 or through Wine/Proton
- Raidcore Nexus
- arcdps configured to save compressed `.zevtc` combat logs
- HTTPS access to each enabled provider

The addon is one native DLL and does not require .NET or a companion runtime DLL. DonBot and Twitch
work under Wine, but Wine DPAPI provides weaker protection than native Windows; the options UI shows
an explicit compatibility warning.

## Installation

Until GW2 Manny Uploader has a public Nexus listing, install a verified Windows artifact manually:

1. Download `GW2-Manny-Uploader-<version>-windows-x64.zip` and its `.sha256` sidecar from the same CI
   or release run.
2. Verify the archive in PowerShell:

   ```powershell
   (Get-FileHash .\GW2-Manny-Uploader-<version>-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\GW2-Manny-Uploader-<version>-windows-x64.zip.sha256
   ```

   The calculated value must match the first value in the sidecar.
3. Confirm the ZIP contains only `manny_uploader.dll`.
4. Copy the DLL directly to `<Guild Wars 2>\addons\manny_uploader.dll`.
5. Launch Guild Wars 2 and load or enable `MannyUploader` through Nexus.

Do not install the separately published PDB or its checksum. Those files are retained only with
release evidence or a matching crash investigation.

## Using the addon

Open the uploader from its quick-access icon or the Nexus-managed `Alt+Shift+M` default keybind. The
keybind can be changed through Nexus. Hovering the icon shows every enabled destination. It is grey
when none are enabled, uses the normal accent when one or more upload providers are enabled, and uses
Twitch purple whenever Twitch reporting is enabled. If Nexus cannot create the optional textures,
the addon remains active through its keybind and options entry.

The main window shows recent logs and each provider's state. `Copy dps.report URLs` copies the
visible report links, while `Copy DonBot aggregate URL` copies one aggregate page for the visible
DonBot fight IDs. Each row can open its source folder. `Reupload` deliberately submits that log to
dps.report, GW2Wingman, and DonBot again; `Rechat` deliberately sends its dps.report link to Twitch
again.

`Show New` is the safe startup default and accepts only logs completed after the addon loaded.
`Show Today` includes logs completed since local midnight plus new logs that arrive afterward. Upload
history is persisted across game restarts, so switching modes or disabling and re-enabling a provider
does not resubmit an already-seen log. Only the explicit `Reupload` and `Rechat` actions replay work.

All configuration is under `MannyUploader` in Nexus options. Saved settings apply without
reloading; active parses and uploads keep the inputs captured when they started.

### General options

| Option | Default | Range or meaning |
| --- | ---: | --- |
| Log directory | `<Guild Wars 2>\arcdps.cbtlogs` | Directory containing `.zevtc` logs; it may be created after startup. |
| Watch subdirectories | On | Include nested encounter directories. |
| Poll interval | 1,000 ms | 250–60,000 ms; native notifications avoid unnecessary scans. |
| Stability observations | 2 | 2–10 unchanged observations before accepting a log. |
| Recent log limit | 50 | 1–500 settled rows retained in memory. |
| Parser queue capacity | 8 | 1–64 queued metadata parses. |
| Parallel uploads per provider | 1 | 1–32 concurrent requests for each destination independently. A value of 10 gives each of dps.report, GW2Wingman, DonBot, and Twitch its own limit of 10. |
| Maximum candidates | 4,096 | 1–10,000 logs inside the selected New/Today window per scan. |

Only `.zevtc` files are supported in version 1. If arcdps writes elsewhere, select that exact
directory. A missing directory is a waiting state and is discovered when it appears.

### dps.report

`Upload to dps.report` is enabled by default. Its result supplies the report link shown in the recent
log table and is required before Twitch posting can be enabled.

### Direct GW2Wingman

`Upload to GW2Wingman` is enabled by default. This uses an isolated raw-EVTC compatibility endpoint
because Wingman's current public workflow is based on processed Elite Insights data. If the
compatibility endpoint is retired, Wingman can fail without blocking the other destinations.

### DonBot

1. Leave the default API URL unless the DonBot operator supplied another HTTPS endpoint.
2. Enter the Guild Wars 2 API key associated with the DonBot account and select `Verify DonBot`.
3. Once verified, the API-key field is hidden. Select one authorized server from the dropdown.
4. Enable uploads with the checkbox beside the server dropdown.

The key is protected separately from ordinary JSON. DonBot always submits with `wingman=false`, so
enabling direct Wingman and DonBot does not ask DonBot to create a duplicate Wingman upload.
`Deverify DonBot` disables the workflow and erases its locally protected key. Completed DonBot
processing IDs are retained so the main window can compose its aggregate URL.

### Twitch broadcaster chat

Twitch uses a public Device Code application and an available protected store. It never asks for or
stores a client secret.

1. Open the [Twitch developer console](https://dev.twitch.tv/console/apps), choose `Register Your
   Application`, use a unique name, add `http://localhost:3000` as the redirect URL if required,
   choose a suitable category, and select the public client type.
2. Open `Manage` for the application, copy its public Client ID into MannyUploader, and save ordinary
   settings. Do not create or paste a Client Secret; MannyUploader does not use one. The `(?)` beside
   the field contains the same setup reminder in-game.
3. Select `Connect Twitch`.
4. Open the displayed verification address and enter the displayed Device Code.
5. Authorize using the broadcaster account whose chat should receive links.
6. Wait for the connected status, enable Twitch chat upload, and use `Send test message`.

`Disconnect Twitch` disables posting, attempts revocation, and erases the protected local session.
Successful and failed encounter posting can be enabled independently, but at least one must remain
selected while Twitch is enabled.

### Twitch message template

The default template is:

```text
{encounter}{mode_suffix} — {result}: {url}
```

| Field | Value |
| --- | --- |
| `{url}` | Trusted dps.report permalink; required. |
| `{encounter}` | Encounter name returned by dps.report. |
| `{mode}` | dps.report mode text. |
| `{mode_suffix}` | Empty when mode is empty, otherwise the mode in parentheses. |
| `{result}` | Exactly `Success` or `Failure`. |
| `{boss_id}` | Unsigned decimal dps.report boss ID. |

Use `{{` or `}}` for a literal brace. Templates must be valid UTF-8, contain `{url}`, and remain
within the displayed limit after expansion. Unknown or unbalanced fields are rejected on save.

## Stored data and credentials

Nexus supplies the addon data directory. The current storage identity produces:

```text
<Guild Wars 2>\addons\GW2MannyUploader\
├── settings.json
├── settings.json.bak
├── upload-history.json
└── secrets\
```

`settings.json` contains ordinary options, including the public Twitch Client ID; `.bak` is its last
known-good document. `upload-history.json` contains log identities, provider states, public report
links, and delivery receipts used to prevent automatic replay. DonBot keys and Twitch OAuth sessions
are bounded DPAPI records under `secrets` and are never written into ordinary JSON. Native Windows
DPAPI is user-scoped. Wine DPAPI encryption does not provide the same protection against a copied
profile or local attacker. Do not edit these files or include them in support bundles.

If the primary settings file is corrupt, the addon attempts the valid backup and reports recovery. If
both existing files are invalid, startup fails closed instead of silently discarding configuration.

## Troubleshooting

### Nexus does not detect the addon

- Confirm the file is exactly `<Guild Wars 2>\addons\manny_uploader.dll`.
- Confirm Nexus is active in the same Guild Wars 2 installation.
- Use the Windows x64 ZIP, not a source archive, Linux module, or PDB.
- Check `<Guild Wars 2>\addons\Nexus\Nexus.log` for the GW2 Manny Uploader channel.

### No logs are detected

- Confirm arcdps creates compressed `.zevtc` files.
- Compare the configured directory with the newest log's location.
- Enable subdirectory watching if arcdps creates encounter-specific folders.
- Use `Show New` for logs completed after addon load or `Show Today` for the current local day.

### DonBot or Twitch cannot be enabled

- DonBot requires a verified key and one authorized guild selection.
- Twitch requires a valid public application Client ID, available protected storage, dps.report, and at least one
  encounter-result policy.
- Do not paste credentials into `settings.json`; secret-like JSON keys are rejected.

### An upload or Twitch message fails

Providers fail independently. Inspect the recent-log row and retry only when the UI offers an
explicit retry. Ambiguous Twitch delivery is not retried automatically because that could create a
duplicate message.

Issue reports should include the addon, Windows, and Nexus versions, safe status text, and
reproduction steps. Never include API keys, OAuth tokens, Device Codes, protected files, raw provider
responses, or unredacted account/path information.

## Updating or removing

Unload or disable the addon through Nexus before replacing or deleting its DLL. A verified update can
replace `manny_uploader.dll` while preserving the addon data directory.

Before permanent removal, disconnect DonBot and Twitch through options so protected records are
erased and Twitch revocation is attempted. After unloading, delete the DLL. Delete the
`GW2MannyUploader` directory only if its settings and remaining records should also be removed.

## Build and test

Development requires CMake 3.25+, Ninja, and a C++23 compiler. MSVC x64 is the production toolchain;
current GCC and Clang builds exercise the portable core.

Dependencies are pinned and hash-verified through CMake: Nexus API v6, compatible Raidcore ImGui
1.80, miniz 3.1.2, Glaze 8.0.0, and static libcurl 8.21.0 with Schannel. A fresh dependency cache
requires network access.

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

Packagers may optionally provide a default public Twitch Client ID at configure time:

```sh
cmake -S . -B out/build/release -DMANNY_TWITCH_CLIENT_ID=yourlowercaseclientid
```

Users can override that fallback—or configure Twitch in a build without one—through Nexus options.

Check source formatting with:

```sh
cmake -DMANNY_SOURCE_DIRECTORY=. -P cmake/CheckSourceFormatting.cmake
```

Windows CI builds and tests Release with MSVC, verifies the CPack ZIP/checksum, runs ten hot-load
cycles against the packaged DLL, and publishes verified linker symbols separately. See the
[native Windows validation matrix](docs/release/native-windows-validation.md) and
[release evidence template](docs/release/evidence-template.md).

## Architecture and development policy

The code is organized into addon, application, configuration, EVTC, filesystem, HTTP, provider, UI,
platform, and support layers. The ownership model and dependency rules are documented in the
[architecture overview](docs/architecture/overview.md); independently frozen behavior is under
[`docs/contracts`](docs/contracts/README.md).

This is a clean-slate implementation. Public service contracts, Nexus ABI documentation, sample
`.zevtc` files, and observable behavior may be used as specifications, but implementation code from
the previous uploader is not copied or translated.

Pinned third-party licenses are reproduced in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Local AI-assistant instructions, prompts, state, and generated context are ignored by Git and must not
be committed.
