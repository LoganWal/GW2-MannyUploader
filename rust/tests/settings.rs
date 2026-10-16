use manny_uploader::domain::*;

fn settings() -> Settings {
    Settings::new("logs/\u{96ea}".into())
}
fn has(settings: &Settings, field: &'static str, code: SettingsErrorCode) -> bool {
    settings
        .validate()
        .contains(&SettingsValidationError { field, code })
}

#[test]
fn schema_defaults_match_the_existing_product_and_provider_selection() {
    let settings = settings();
    assert!(settings.validate().is_empty());
    assert_eq!(settings.schema_version, 1);
    assert!(settings.general.watch_subdirectories && settings.general.window_visible);
    assert_eq!(
        (
            settings.general.poll_interval_ms,
            settings.general.stability_observations,
            settings.general.recent_log_limit
        ),
        (1000, 2, 100)
    );
    assert_eq!(
        (
            settings.general.parser_queue_capacity,
            settings.general.parallel_uploads_per_provider,
            settings.general.max_candidates
        ),
        (8, 5, 4096)
    );
    assert!(!settings.dps_report.detailed_wvw);
    assert_eq!(
        settings.enabled_providers(),
        ProviderSelection {
            dps_report: true,
            wingman: true,
            donbot: false,
            twitch: false
        }
    );
    assert!(
        !settings.donbot.discord_delivery_enabled
            && !settings.donbot.discord_channel_override_explicit
    );
    assert_eq!(
        settings.donbot.api_base_url,
        "https://donbot-api.walmslo.com"
    );
    assert!(settings.twitch.client_id.is_empty());
    assert_eq!(
        settings.twitch.message_template,
        "{encounter}{mode_suffix} - {result}: {url}"
    );
}

#[test]
fn paths_schema_and_all_numeric_boundaries_are_validated_without_echoing_values() {
    let mut value = settings();
    value.schema_version = 2;
    value.general.log_directory = "private\0path".into();
    let errors = value.validate();
    assert!(has(
        &value,
        "schema_version",
        SettingsErrorCode::UnsupportedSchema
    ));
    assert!(has(
        &value,
        "general.log_directory",
        SettingsErrorCode::InvalidPath
    ));
    assert!(!format!("{errors:?}").contains("private"));
    value = settings();
    value.general.log_directory = "x".repeat(4096);
    assert!(value.validate().is_empty());
    value.general.log_directory.push('x');
    assert!(has(
        &value,
        "general.log_directory",
        SettingsErrorCode::ValueTooLong
    ));
    value.general.log_directory.clear();
    assert!(has(
        &value,
        "general.log_directory",
        SettingsErrorCode::EmptyValue
    ));
    type Field = fn(&mut GeneralSettings) -> &mut u32;
    let fields: [(Field, u32, u32, &str); 6] = [
        (
            |g| &mut g.poll_interval_ms,
            250,
            60_000,
            "general.poll_interval_ms",
        ),
        (
            |g| &mut g.stability_observations,
            2,
            10,
            "general.stability_observations",
        ),
        (
            |g| &mut g.recent_log_limit,
            1,
            500,
            "general.recent_log_limit",
        ),
        (
            |g| &mut g.parser_queue_capacity,
            1,
            64,
            "general.parser_queue_capacity",
        ),
        (
            |g| &mut g.parallel_uploads_per_provider,
            1,
            32,
            "general.parallel_uploads_per_provider",
        ),
        (
            |g| &mut g.max_candidates,
            1,
            10_000,
            "general.max_candidates",
        ),
    ];
    for (field, min, max, name) in fields {
        for (number, valid) in [
            (min - 1, false),
            (min, true),
            (max, true),
            (max + 1, false),
            (u32::MAX, false),
        ] {
            let mut value = settings();
            *field(&mut value.general) = number;
            assert_eq!(!has(&value, name, SettingsErrorCode::OutOfRange), valid);
        }
    }
}

#[test]
fn donbot_url_rejects_unsafe_components_and_requires_a_host_and_valid_port() {
    for url in [
        "https://example.test",
        "https://example.test/api/",
        "https://localhost:443/api",
        "https://[::1]:8443/api",
    ] {
        let mut value = settings();
        value.donbot.api_base_url = url.into();
        assert!(value.validate().is_empty());
    }
    for url in [
        "",
        "http://example.test",
        "https://",
        "https:///api",
        "https://user:secret@example.test",
        "https://example.test?q=secret",
        "https://example.test/#fragment",
        "https://bad host",
        "https://example.test\\other",
        "https://example.test/../api",
        "https://example.test/.",
        "https://:443",
        "https://host:",
        "https://host:0",
        "https://host:65536",
        "https://[bad]",
        "https://[::1]suffix",
        "https://host\n",
        "https://host\u{2003}name",
    ] {
        let mut value = settings();
        value.donbot.api_base_url = url.into();
        assert!(has(
            &value,
            "donbot.api_base_url",
            SettingsErrorCode::InvalidUrl
        ));
    }
    let mut value = settings();
    value.donbot.api_base_url = format!("https://example.test/{}", "x".repeat(2048));
    assert!(has(
        &value,
        "donbot.api_base_url",
        SettingsErrorCode::InvalidUrl
    ));
}

#[test]
fn donbot_requires_canonical_ids_and_explicit_discord_overrides_survive_loading() {
    let mut value = settings();
    value.donbot.enabled = true;
    assert!(has(
        &value,
        "donbot.selected_guild_id",
        SettingsErrorCode::EmptyValue
    ));
    for id in ["0", "01", "+1", "-1", "123x", " 123", "9223372036854775808"] {
        value.donbot.selected_guild_id = id.into();
        value.donbot.selected_discord_channel_id = id.into();
        assert!(has(
            &value,
            "donbot.selected_guild_id",
            SettingsErrorCode::InvalidGuildId
        ));
        assert!(has(
            &value,
            "donbot.selected_discord_channel_id",
            SettingsErrorCode::InvalidChannelId
        ));
    }
    value.donbot.selected_guild_id = i64::MAX.to_string();
    value.donbot.selected_discord_channel_id = "123".into();
    value.donbot.discord_delivery_enabled = true;
    assert!(value.validate().is_empty());
    value.normalize_loaded_settings();
    assert!(value.donbot.selected_discord_channel_id.is_empty());
    value.donbot.discord_channel_override_explicit = true;
    value.donbot.selected_discord_channel_id = "123".into();
    value.normalize_loaded_settings();
    assert_eq!(value.donbot.selected_discord_channel_id, "123");
    value.donbot.enabled = false;
    assert!(has(
        &value,
        "donbot.discord_delivery_enabled",
        SettingsErrorCode::DiscordRequiresDonBot
    ));
}

#[test]
fn twitch_dependencies_client_ids_and_templates_use_one_validation_boundary() {
    let mut value = settings();
    value.twitch.enabled = true;
    value.dps_report.enabled = false;
    value.twitch.post_success = false;
    value.twitch.post_failure = false;
    assert!(has(
        &value,
        "twitch.enabled",
        SettingsErrorCode::TwitchRequiresDpsReport
    ));
    assert!(has(
        &value,
        "twitch.enabled",
        SettingsErrorCode::TwitchPostingDisabled
    ));
    value = settings();
    value.twitch.client_id = "a".repeat(128);
    assert!(value.validate().is_empty());
    for id in [
        "a".repeat(129),
        "PUBLIC".into(),
        "a-b".into(),
        "\u{96ea}".into(),
    ] {
        value.twitch.client_id = id;
        assert!(has(
            &value,
            "twitch.client_id",
            SettingsErrorCode::InvalidTwitchClientId
        ));
    }
    for template in [
        "{{url}}",
        "{unknown}{url}",
        "{}",
        "{encounter}",
        "{url",
        "\n{url}",
    ] {
        value = settings();
        value.twitch.message_template = template.into();
        assert!(has(
            &value,
            "twitch.message_template",
            SettingsErrorCode::Template(TwitchMessageTemplate::parse(template).unwrap_err())
        ));
    }
}

#[test]
fn loading_disabled_donbot_clears_even_explicit_stale_channel_selection() {
    let mut value = settings();
    value.donbot.discord_channel_override_explicit = true;
    value.donbot.selected_discord_channel_id = "123".into();
    value.normalize_loaded_settings();
    assert!(!value.donbot.discord_channel_override_explicit);
    assert!(value.donbot.selected_discord_channel_id.is_empty());
}
