# Settings schema version 1

This contract defines ordinary, user-editable configuration for GW2 Manny Uploader. The JSON file is
not a secret store. It may be copied into diagnostics after path/account identifiers are redacted.

## Document shape

```json
{
  "schema_version": 1,
  "general": {
    "log_directory": "C:/Users/example/Documents/Guild Wars 2/addons/arcdps/arcdps.cbtlogs",
    "watch_subdirectories": true,
    "window_visible": true,
    "poll_interval_ms": 1000,
    "stability_observations": 2,
    "recent_log_limit": 100,
    "parser_queue_capacity": 8,
    "parallel_uploads_per_provider": 5,
    "max_candidates": 4096
  },
  "dps_report": {
    "enabled": true,
    "detailed_wvw": false
  },
  "wingman": {
    "enabled": true
  },
  "donbot": {
    "enabled": false,
    "api_base_url": "https://donbot-api.walmslo.com",
    "selected_guild_id": "",
    "discord_delivery_enabled": false,
    "discord_channel_override_explicit": false,
    "selected_discord_channel_id": ""
  },
  "twitch": {
    "enabled": false,
    "client_id": "",
    "message_template": "{encounter}{mode_suffix} — {result}: {url}",
    "post_success": true,
    "post_failure": true
  }
}
```

`schema_version` is required and must be exactly `1`. Missing nested objects or fields receive the
current version-1 defaults, allowing defaults to be added without a migration. Unknown keys are
rejected at every level so misspellings and accidentally persisted secrets are visible failures rather
than silently ignored. JSON comments and trailing non-whitespace content are rejected. All strings
must be valid UTF-8.

## Validation

- `log_directory` is non-empty UTF-8 and at most 4096 bytes. Existence is a runtime status, not a save
  requirement, because the polling source supports a directory created after startup.
- `poll_interval_ms`: 250 through 60,000.
- `stability_observations`: 2 through 10.
- `recent_log_limit`: 1 through 500.
- `parser_queue_capacity`: 1 through 64.
- `parallel_uploads_per_provider`: 1 through 32. The limit is applied independently to every
  provider worker and may be changed live.
- `dps_report.detailed_wvw` defaults to false. When true, newly accepted dps.report work requests
  detailed WvW parsing. Already queued uploads keep the value captured when they were accepted.
- `max_candidates`: 1 through 10,000.
- `donbot.api_base_url` is an HTTPS base URL without credentials, query, fragment, whitespace, or an
  empty host and is at most 2048 bytes.
- `donbot.selected_guild_id` is empty while DonBot is disabled or is a canonical positive decimal
  value within a signed 64-bit integer. Enabling DonBot requires a verified guild selection.
- `donbot.discord_delivery_enabled` defaults to false and requires DonBot uploads to be enabled.
- `donbot.discord_channel_override_explicit` defaults to false. Guild defaults are used unless the
  user explicitly selects a channel. An older persisted channel ID without this marker is stale and
  is cleared during loading.
- `donbot.selected_discord_channel_id` is empty for DonBot server defaults or is a canonical
  positive decimal value within a signed 64-bit integer. An ID is submitted only while the explicit
  override marker is true and the DonBot workflow has verified that the channel remains authorized.
- Twitch can be enabled only while dps.report is enabled and at least one posting policy is enabled.
- `twitch.client_id` is empty or a public Twitch application Client ID of at most 128 lowercase
  ASCII letters and digits. It is ordinary, non-secret configuration and may change only while the
  Twitch workflow is disconnected or in error.
- Twitch templates are non-empty valid UTF-8 without ASCII controls and at most 500 bytes. Supported placeholders are
  `{url}`, `{encounter}`, `{mode}`, `{mode_suffix}`, `{result}`, and `{boss_id}`. `{url}` is required.
  `{{` and `}}` encode literal braces; unknown, empty, or unbalanced placeholders are invalid.

Direct GW2Wingman and DonBot may both be enabled. This does not permit duplicate Wingman forwarding:
the DonBot provider contract always requests `wingman=false`.

## Secrets forbidden from JSON

The ordinary settings types deliberately have no fields for:

- dps.report user tokens;
- DonBot/Guild Wars 2 API keys;
- Twitch access or refresh tokens;
- OAuth device codes; or
- client secrets.

The public Twitch application Client ID is not a credential and is deliberately stored in ordinary
JSON. It is never sufficient to authenticate or post without the separately protected OAuth session.

An attempted secret-like JSON key is therefore rejected as unknown. Protected credentials use a
separate `ISecretStore` adapter owned by the application `ConfigurationService` and follow their own
lifecycle contract.

## File behavior

The settings document is limited to 64 KiB before parsing. Saving validates the in-memory settings,
writes a sibling temporary file, flushes it, and atomically replaces the primary file. A valid previous
primary is atomically copied to a `.bak` last-known-good file before replacement; an invalid primary
never overwrites a valid backup.

Loading tries the primary first, then the backup. Defaults are returned only when neither file exists.
A recovered backup is reported to the caller with the primary failure diagnostic; recovery is never
silent. If files exist but neither is valid, loading returns an error rather than silently resetting
the user's configuration. The store has one runtime owner; concurrent processes or addon instances
must not write the same settings path.
