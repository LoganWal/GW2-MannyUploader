use manny_uploader::config::*;
use manny_uploader::domain::*;

const MINIMAL: &[u8] = br#"{"schema_version":1,"general":{"log_directory":"D:/logs"}}"#;

#[test]
fn minimal_documents_receive_version_one_defaults_and_schema_is_mandatory() {
    assert_eq!(
        decode_settings(MINIMAL).unwrap(),
        Settings::new("D:/logs".into())
    );
    assert_eq!(decode_settings(br#"{"schema_version":1,"general":{"log_directory":"D:/logs"},"dps_report":{},"wingman":{},"donbot":{},"twitch":{}}"#).unwrap(), Settings::new("D:/logs".into()));
    assert_eq!(
        decode_settings(br#"{"general":{"log_directory":"D:/logs"}}"#),
        Err(SettingsDocumentError::InvalidJson)
    );
    assert!(matches!(
        decode_settings(br#"{"schema_version":2,"general":{"log_directory":"D:/logs"}}"#),
        Err(SettingsDocumentError::InvalidSettings(_))
    ));
    assert!(matches!(
        decode_settings(br#"{"schema_version":1}"#),
        Err(SettingsDocumentError::InvalidSettings(_))
    ));
}

#[test]
fn every_field_round_trips_and_output_is_pretty_utf8_with_no_credential_keys() {
    let mut settings = Settings::new("D:/\u{96ea}/logs".into());
    settings.general.watch_subdirectories = false;
    settings.general.window_visible = false;
    settings.general.poll_interval_ms = 250;
    settings.general.stability_observations = 10;
    settings.general.recent_log_limit = 500;
    settings.general.parser_queue_capacity = 64;
    settings.general.parallel_uploads_per_provider = 32;
    settings.general.max_candidates = 10_000;
    settings.dps_report.detailed_wvw = true;
    settings.wingman.enabled = false;
    settings.donbot.enabled = true;
    settings.donbot.api_base_url = "https://example.test/v1/".into();
    settings.donbot.selected_guild_id = "123".into();
    settings.donbot.discord_delivery_enabled = true;
    settings.donbot.discord_channel_override_explicit = true;
    settings.donbot.selected_discord_channel_id = "456".into();
    settings.twitch.enabled = true;
    settings.twitch.client_id = "public123".into();
    settings.twitch.message_template = "\u{96ea} {url}".into();
    settings.twitch.post_failure = false;
    let encoded = encode_settings(&settings).unwrap();
    assert_eq!(decode_settings(&encoded).unwrap(), settings);
    let text = std::str::from_utf8(&encoded).unwrap();
    assert!(
        text.ends_with('\n') && text.contains("\n  \"general\": {") && text.contains('\u{96ea}')
    );
    for key in [
        "access_token",
        "refresh_token",
        "api_key",
        "user_token",
        "device_code",
        "client_secret",
    ] {
        assert!(!text.contains(key));
    }
}

#[test]
fn unknown_or_secret_keys_fail_at_every_level_without_echoing_keys_or_values() {
    for object in [
        None,
        Some("general"),
        Some("dps_report"),
        Some("wingman"),
        Some("donbot"),
        Some("twitch"),
    ] {
        for key in [
            "typo",
            "api_key",
            "userToken",
            "access_token",
            "refresh_token",
            "device_code",
            "client_secret",
        ] {
            let property = format!("\"{key}\":\"secret-marker\"");
            let document = match object {
                None => format!(
                    "{{\"schema_version\":1,\"general\":{{\"log_directory\":\"D:/logs\"}},{property}}}"
                ),
                Some("general") => format!(
                    "{{\"schema_version\":1,\"general\":{{\"log_directory\":\"D:/logs\",{property}}}}}"
                ),
                Some(name) => format!(
                    "{{\"schema_version\":1,\"general\":{{\"log_directory\":\"D:/logs\"}},\"{name}\":{{{property}}}}}"
                ),
            };
            let error = decode_settings(document.as_bytes()).unwrap_err();
            assert_eq!(error, SettingsDocumentError::InvalidJson);
            assert!(!format!("{error:?}").contains("secret-marker"));
        }
    }
}

#[test]
fn duplicates_nulls_arrays_comments_trailing_text_and_invalid_utf8_are_rejected() {
    for document in [
        r#"{"schema_version":1,"schema_version":1,"general":{"log_directory":"D:/logs"}}"#,
        r#"{"schema_version":1,"general":{"log_directory":"D:/logs","log_directory":"D:/other"}}"#,
        r#"{"schema_version":1,"general":{"log_directory":"D:/logs"},"twitch":{},"twitch":{}}"#,
        r#"{"schema_version":1,"general":{"log_directory":null}}"#,
        r#"{"schema_version":1,"general":{"log_directory":"D:/logs"},"wingman":[true]}"#,
        r#"{"schema_version":1,"general":{"log_directory":"D:/logs"},"twitch":null}"#,
        r#"[1,{"log_directory":"D:/logs"},{},{},{},{}]"#,
        r#"{"schema_version":1,/* comment */"general":{"log_directory":"D:/logs"}}"#,
        r#"{"schema_version":1,"general":{"log_directory":"D:/logs"}} trailing"#,
        r#"{"schema_version":1,"general":{"log_directory":"D:/logs","poll_interval_ms":1000.0}}"#,
        r#"{"schema_version":1,"general":{"log_directory":"\uD800"}}"#,
    ] {
        assert_eq!(
            decode_settings(document.as_bytes()),
            Err(SettingsDocumentError::InvalidJson)
        );
    }
    let mut invalid_utf8 = br#"{"schema_version":1,"general":{"log_directory":""#.to_vec();
    invalid_utf8.extend_from_slice(&[0xc0, 0x80]);
    invalid_utf8.extend_from_slice(br#""}}"#);
    assert_eq!(
        decode_settings(&invalid_utf8),
        Err(SettingsDocumentError::InvalidJson)
    );
}

#[test]
fn document_and_recursion_limits_precede_typed_values_and_serialization_validates() {
    let mut exact = MINIMAL.to_vec();
    exact.resize(MAX_SETTINGS_BYTES, b' ');
    assert!(decode_settings(&exact).is_ok());
    exact.push(b' ');
    assert_eq!(
        decode_settings(&exact),
        Err(SettingsDocumentError::TooLarge)
    );
    let nested = format!("{{\"nested\":{}{}}}", "[".repeat(200), "]".repeat(200));
    assert_eq!(
        decode_settings(nested.as_bytes()),
        Err(SettingsDocumentError::InvalidJson)
    );
    let mut settings = Settings::new("D:/logs".into());
    settings.twitch.enabled = true;
    settings.dps_report.enabled = false;
    assert!(matches!(
        encode_settings(&settings),
        Err(SettingsDocumentError::InvalidSettings(_))
    ));
}

#[test]
fn legacy_and_disabled_channel_overrides_are_cleared_during_loading() {
    for donbot in [
        r#"{"enabled":true,"selected_guild_id":"123","selected_discord_channel_id":"456"}"#,
        r#"{"enabled":false,"discord_channel_override_explicit":true,"selected_discord_channel_id":"456"}"#,
    ] {
        let document = format!(
            "{{\"schema_version\":1,\"general\":{{\"log_directory\":\"D:/logs\"}},\"donbot\":{donbot}}}"
        );
        let settings = decode_settings(document.as_bytes()).unwrap();
        assert!(!settings.donbot.discord_channel_override_explicit);
        assert!(settings.donbot.selected_discord_channel_id.is_empty());
    }
}
