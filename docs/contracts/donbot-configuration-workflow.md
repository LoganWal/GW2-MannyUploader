# DonBot configuration-workflow contract

This contract defines the application-owned workflow between Nexus options, asynchronous DonBot key
verification, ordinary settings, and protected key persistence. No Nexus render/options callback may
perform HTTP or protected-storage I/O directly.

## Command and snapshot boundary

`IDonBotVerifier` accepts a move-only command containing a stable request ID, normalized HTTPS API
base, and candidate `SecretValue`. It returns the matching ID plus either verified account/guild data
and the same move-only key, or a typed generic failure. The bounded production worker owns one joined
thread, calls `IDonBotClient::verify` off the application thread, converts every non-cancellation
client outcome to a visible verification failure, and performs no automatic retry.

`DonBotConfigurationController` is single-owner application state. Its immutable snapshots expose
only:

- `Unverified`, `Verifying`, `Verified`, `Error`, or `ShuttingDown` state;
- normalized API base;
- transient verified account name and authorized guild list;
- negotiated Discord delivery capability, guild policy, message kinds, and authorized channels
- the durably selected guild ID;
- safe diagnostic, revision, and shutdown state.

Snapshots never contain the API key, a key prefix, credential presence, a request body, or any other
secret-derived marker. Verified identity and authorized channel choices exist only in memory after a
successful current verification. Only the selected guild ID, delivery enablement, explicit override
marker, and selected channel ID are ordinary persisted configuration.

Only one verification may be active. Results are correlated by monotonically increasing request ID;
an unmatched result is discarded as stale without ending the current request. Shutdown clears queued
commands/results and transient identity, requests cooperative cancellation, and rejects later calls.

## Candidate-key persistence ordering

Starting verification validates and normalizes the supplied API base, moves the candidate key into
the verifier, and performs no persistence. Verification failure leaves the existing ordinary
settings and protected record unchanged.

After successful verification, persistence occurs in this exact order:

1. Save the verified API base with DonBot disabled and the selected guild cleared.
2. Replace the protected `DonBotGw2ApiKey` record.
3. Publish the transient verified account and authorized guilds.

A settings failure therefore cannot replace the old key. A protected-key failure may leave the new
base persisted, but DonBot is durably disabled with no selected guild. Verified state is never
published until both operations succeed.

Guild selection is allowed only in current `Verified` state, must exactly match an authorized guild
returned by DonBot, and must still refer to the same configured API base. Selection is saved before
the snapshot changes. Selection does not implicitly enable DonBot; the options presenter controls the
ordinary enable toggle separately after a durable verified selection exists.

Changing the selected guild disables Discord delivery and clears its channel override. Disabling
DonBot or Discord delivery also clears the override. Selecting the same guild preserves the current
route. Enabling delivery requires DonBot uploads, negotiated `discord-summary-delivery-v1`, an
enabled guild policy, and an authorized route. Guild defaults are the initial route. A channel
override is used only after an explicit channel selection and must match a channel returned for the
selected guild. Selecting guild defaults clears both the explicit marker and channel ID. Saves
complete before the snapshot revision advances.

## Startup and disconnect

Startup verification explicitly loads `DonBotGw2ApiKey` through `ConfigurationService` and moves it
to the verifier. A successful result does not rewrite the same protected key. An existing selected
guild is retained only when it is in the returned authorized set. If it is no longer authorized,
ordinary settings are first saved with DonBot disabled and the guild, Discord delivery, and channel
selection cleared, then verified identity is published.

Startup preserves Discord delivery only while the capability, guild policy, and explicitly selected
channel remain authorized. A persisted channel ID without the explicit marker is stale, is cleared,
and resolves to guild defaults. Revocation durably disables delivery and clears the channel before
publishing the verified snapshot. Ordinary DonBot uploads remain enabled when the guild is still
authorized.
The provider starts with no Discord delivery route. Discovery waits while saved delivery
verification is active, and the route is published to the provider only from a matching current
verified snapshot. A failed verification cannot send to a persisted guild or channel.

If ordinary settings change to a different API base while saved-key verification is running, the
result is stale and cannot authorize selection. Missing or unavailable protected storage becomes a
typed visible error without a network command.

Disconnect is ordered for fail-safe behavior:

1. Durably disable DonBot and clear its selected guild, Discord delivery, and selected channel.
2. Clear transient verified identity.
3. Erase the protected key.

If the settings save fails, erase is not attempted. If erase fails, DonBot remains durably disabled
and the failure is visible. Disconnect cannot race active verification.

## Deterministic tests

Owned-fake tests cover success and secret-free snapshots, URL normalization, exact save-before-store
and save-before-erase ordering, no persistence before verification, authorized selection, settings
and secret failures, saved-key startup, revoked guilds, stale IDs and settings, queue rejection,
cancellation, exception containment, backpressure, and idempotent joined shutdown.
