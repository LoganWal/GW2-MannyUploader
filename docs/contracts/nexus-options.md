# Nexus options command boundary

- Status: Implemented application, view-model, and Nexus/ImGui adapter boundary
- Date: 2026-08-21
- Scope: ordinary settings plus DonBot and broadcaster-owned Twitch configuration and test delivery

## Ownership and thread boundary

`NexusOptionsController` is the sole bridge between a Nexus options renderer and the application
configuration workflows. The renderer receives a deep-copy `NexusOptionsSnapshot` and may submit a
move-owned `NexusOptionsCommand`. It does not receive `ConfigurationService`, either workflow
controller, a persistence adapter, an HTTP client, or a worker port.

The render callback may only:

1. read a published snapshot;
2. build the pure `NexusOptionsModel`;
3. edit adapter-owned bounded input buffers; and
4. submit a value command to the bounded queue.

Submission performs in-memory validation, one short mutex acquisition, and queue insertion. It does
not save settings, access protected storage, enqueue provider work, poll OAuth, or perform HTTP. The
background application owner calls `tick()` outside rendering. A tick drains at most the configured
command limit in FIFO order, performs write-through configuration actions, starts asynchronous
verification or authentication operations, polls at most one result from each workflow, and
publishes a new snapshot. Twitch test-message HTTP work runs only in its dedicated worker; a tick only
queues or polls that worker.

## Commands

Ordinary editing is represented by `SaveOrdinaryOptionsCommand`. Its payload contains general,
public Twitch Client ID, and Twitch message-policy fields. Applying it preserves the current
dps.report, GW2Wingman, DonBot, and Twitch enabled states.
Once that command has durably saved and published a new settings revision, the production application
owner applies general polling, stability, parser-capacity, per-provider parallelism, and recent-history
values to the existing components. No render callback performs this reconfiguration, and active
parse/upload inputs are not rewritten. The public Twitch Client ID can change only while disconnected
or in error; no secret is needed for Device Code authorization.
A dedicated `SetWindowVisibleCommand` updates only the persisted window visibility bit. Dedicated
dps.report, Detailed WvW, and GW2Wingman commands update only their respective setting. Disabling
dps.report also disables Twitch posting because Twitch requires a same-job permalink. It does not
disconnect or erase the Twitch session. The options checkbox, window close button, Nexus input bind,
and quick-access shortcut all use the narrow visibility path so they cannot replay a stale
ordinary-options draft over unrelated settings.

The dps.report, Detailed WvW, and GW2Wingman narrow toggles appear in both the main window and Nexus
options. DonBot upload, Discord delivery, guild, and channel controls appear in the main window as
well as their verified options workflow. The main-window guild and Discord route row is hidden while
DonBot is not selected as an upload destination. The Discord route appears when the selected guild
advertises a usable delivery policy, so a required channel override can be chosen before Discord
summaries are enabled. When the selected guild does not advertise a usable delivery policy, the main
window shows a concise reason instead of disabled checkbox and channel controls.

Workflow-owned values use dedicated commands:

- enable or disable dps.report.
- enable or disable Detailed WvW parsing for new dps.report uploads.
- enable or disable GW2Wingman.
- verify a candidate DonBot endpoint/key.
- select a guild returned by the current verified DonBot identity.
- enable or disable DonBot.
- enable or disable DonBot Discord summaries.
- select DonBot guild defaults or an authorized Discord channel.
- disconnect DonBot and erase its protected key.
- begin broadcaster-owned Twitch Device Code authentication.
- enable or disable Twitch posting.
- send one explicit test message to the connected broadcaster's own chat.
- disconnect Twitch and revoke/erase its session.
- dismiss the last options error.

DonBot can be enabled only while the current verified endpoint still matches ordinary settings and
the selected guild remains in the verified authorized-guild set. Twitch can be enabled only while
the authentication workflow is connected. Final settings validation still runs inside
`ConfigurationService`, so enabling Twitch also requires dps.report and at least one post-result
policy.

Automatic Discord summary delivery can be enabled only while DonBot uploads are enabled and the selected guild
advertises `discord-summary-delivery-v1`. Guild defaults are selected initially. Route commands
accept empty for guild defaults or an exact channel ID from the current verified guild. Only an
explicit channel command sets the persisted override marker. These workflow-owned values are
excluded from stale ordinary-options drafts. Turning automatic delivery off preserves an authorized
route for explicit aggregate delivery. Disabling DonBot, changing guild, deverifying, or losing
authorization clears an invalid route.

There is no command or settings field for a Twitch channel, broadcaster ID, sender ID, raw token, or
client secret. The ordinary Client ID identifies the public application; the Twitch workflow's
validated user remains both broadcaster and sender.

The test-message command has no payload. Application execution requires the Twitch workflow to be
`Connected`, permits one in-flight request, assigns a correlated ID, and queues that ID through
`ITwitchTestMessenger`. The worker constructs fixed text containing the request ID. It does not use
the encounter template and cannot fabricate a log, upload job, dps.report result, or target channel.
Safe-retry and ambiguous results become visible terminal errors; no automatic test-message resend is
scheduled.

## Queue and diagnostics

The command queue is bounded, defaults to 32 entries, and rejects overflow instead of blocking.
Application ticks default to at most eight commands. Queue limits must be non-zero, the tick limit
must not exceed capacity, and capacity is capped at 256.

Malformed ordinary settings and DonBot candidates are rejected before insertion. Rejection publishes
a safe error immediately. Workflow or persistence failures encountered by the application owner are
recorded as a safe last error and retain typed underlying categories where available. Errors contain
no credential bytes, raw provider documents, HTTP values, or chat messages.

## Snapshot and view model

`NexusOptionsSnapshot` deep-copies:

- the existing secret-free configuration snapshot;
- the DonBot account/guild workflow snapshot;
- the Twitch public connection snapshot;
- the correlated Twitch test-message state, safe diagnostic, normalized delivery status, and
  ambiguity flag;
- a safe last error;
- pending-command count, revision, acceptance, and shutdown state.

It never contains a DonBot key, Twitch device code, access token, refresh token, or encoded session.
The public Twitch user code and activation URI are intentionally visible while authorization is
pending.

`build_nexus_options_model` is a pure mapping used to test status text and control availability
without ImGui. It disables credential-dependent controls when protected storage is unavailable,
exposes verified/connected account labels, and disables every mutating control once command
acceptance stops. DonBot delivery controls require an enabled upload destination and verified guild
policy. The test-message control is enabled only for a connected broadcaster while no test request
is being sent.

## Shutdown

`shutdown()` first stops command acceptance and destroys all queued commands. Destruction of a queued
move-only `SecretValue` wipes its storage. Shutdown cancels the test-message port once so queued or
in-flight chat work observes addon shutdown; it performs no persistence, authentication, or
revocation itself. The operation is idempotent. The addon lifecycle closes callback admission,
deregisters callbacks, waits for admitted renders, and then stops the remaining DonBot/Twitch
workflows and joins their workers before unload returns.

## Required tests

Portable deterministic tests must prove:

- submission alone causes no settings, protected-storage, verifier, or authenticator activity;
- window visibility commands preserve every unrelated setting;
- FIFO command drainage and per-tick limits;
- queue overflow and malformed input rejection;
- DonBot verify/select/enable/disconnect rules and persistence ordering;
- Twitch connect/enable/disconnect rules using the authenticated broadcaster only;
- Twitch test-message render-boundary isolation, connected/in-flight gating, correlation, typed
  terminal status, redaction, and cancellation;
- protected-storage capability and workflow states drive view-model enablement;
- keys and tokens never appear in snapshots or view models; and
- shutdown clears queued secret-bearing commands without dispatch.
- destination toggle commands preserve unrelated settings and stale ordinary drafts cannot overwrite
  destination state.
