use manny_uploader::{application::*, domain::*, ui::*};

fn snapshot() -> NexusOptionsSnapshot {
    NexusOptionsSnapshot {
        configuration: ConfigurationSnapshot {
            settings: Settings::new("synthetic-logs".into()),
            settings_load_source: SettingsLoadSource::Primary,
            settings_recovery: None,
            persistent_secret_storage: PersistentSecretStorageSnapshot {
                state: PersistentSecretStorageState::Available,
                mode: Some(SecretProtectionMode::NativeUserScoped),
                error_code: None,
                system_error: None,
            },
            revision: 1,
            shutting_down: false,
        },
        donbot: DonBotConfigurationSnapshot {
            state: DonBotConfigurationState::Unverified,
            api_base_url: DEFAULT_DONBOT_API.into(),
            account_name: None,
            discord_summary_delivery_v1: false,
            discord_aggregate_delivery_v1: false,
            guilds: vec![],
            selected_guild_id: String::new(),
            diagnostic: None,
            refreshing: false,
            revision: 1,
            shutting_down: false,
        },
        twitch: TwitchConnectionSnapshot {
            state: TwitchConnectionState::Disconnected,
            login: None,
            user_code: None,
            verification_uri: None,
            authorization_expires_at: None,
            access_expires_at: None,
            diagnostic: None,
            revision: 1,
            shutting_down: false,
        },
        twitch_test_message: TwitchTestMessageSnapshot {
            state: TwitchTestMessageState::Idle,
            request_id: None,
            delivery_status: None,
            diagnostic: None,
            revision: 1,
        },
        last_error: None,
        pending_commands: 0,
        revision: 1,
        accepting_commands: true,
        shutting_down: false,
    }
}
fn connected() -> NexusOptionsSnapshot {
    let mut state = snapshot();
    state.configuration.settings.donbot.enabled = true;
    state.configuration.settings.donbot.selected_guild_id = "123".into();
    state.donbot.state = DonBotConfigurationState::Verified;
    state.donbot.account_name = Some("Player.1234".into());
    state.donbot.selected_guild_id = "123".into();
    state.donbot.discord_summary_delivery_v1 = true;
    state.donbot.discord_aggregate_delivery_v1 = true;
    state.donbot.guilds.push(DonBotGuild {
        guild_id: "123".into(),
        guild_name: "Guild ## One".into(),
        discord_delivery: DonBotDiscordDeliveryPolicy {
            enabled: true,
            defaults_available: true,
            channel_override_allowed: true,
            pve_summary: true,
            aggregate_enabled: true,
            aggregate_defaults_available: true,
            channels: vec![DonBotChannel {
                channel_id: "456".into(),
                channel_name: "logs".into(),
            }],
            ..Default::default()
        },
    });
    state.twitch.state = TwitchConnectionState::Connected;
    state.twitch.login = Some("broadcaster".into());
    state
}
fn mutations(model: &NexusOptionsModel) -> [bool; 14] {
    [
        model.settings_editable,
        model.save_available,
        model.donbot.verify_available,
        model.donbot.guild_selection_available,
        model.donbot.enable_toggle_available,
        model.donbot.refresh_available,
        model.donbot.discord_delivery_toggle_available,
        model.donbot.discord_channel_selection_available,
        model.donbot.disconnect_available,
        model.twitch.connect_available,
        model.twitch.authorization_available,
        model.twitch.enable_toggle_available,
        model.twitch.disconnect_available,
        model.twitch.test_message_available,
    ]
}

#[test]
fn disconnected_controls_and_snapshot_are_pure() {
    let state = snapshot();
    let before = state.clone();
    let model = build_nexus_options_model(&state);
    assert_eq!(state, before);
    assert_eq!(model, build_nexus_options_model(&state));
    assert_eq!(model.donbot.status_text, "Not verified");
    assert!(model.donbot.verify_available);
    assert!(!model.donbot.configuration_visible);
    assert!(!model.donbot.disconnect_available);
    assert!(!model.donbot.enable_toggle_available);
    assert_eq!(model.twitch.status_text, "Not connected");
    assert!(model.twitch.connect_available);
    assert!(!model.twitch.test_message_available);
    assert!(!model.command_pending);
    assert!(model.settings_editable && model.save_available);
}

#[test]
fn connected_controls_follow_live_destinations_and_pending_commands() {
    let mut state = connected();
    state.pending_commands = 2;
    let model = build_nexus_options_model(&state);
    assert!(model.command_pending);
    assert_eq!(model.donbot.status_text, "Verified as Player.1234");
    assert!(!model.donbot.verify_available);
    assert!(model.donbot.configuration_visible && model.donbot.guild_selection_available);
    assert!(model.donbot.enable_toggle_available && model.donbot.refresh_available);
    assert!(model.donbot.disconnect_available && model.donbot.discord_delivery_visible);
    assert!(model.donbot.discord_channel_selection_available);
    assert_eq!(model.twitch.status_text, "Connected as broadcaster");
    assert!(model.twitch.enable_toggle_available && model.twitch.test_message_available);
    state.configuration.settings.donbot.enabled = false;
    let disabled = build_nexus_options_model(&state);
    assert!(!disabled.donbot.configuration_visible);
    assert!(!disabled.donbot.discord_delivery_visible);
    assert!(!disabled.donbot.discord_channel_selection_visible);
    assert!(disabled.donbot.enable_toggle_available);
    assert!(disabled.donbot.disconnect_available);
}

#[test]
fn verification_and_refresh_disable_mutations_and_keep_refresh_identity() {
    let mut state = connected();
    state.donbot.refreshing = true;
    let model = build_nexus_options_model(&state).donbot;
    assert_eq!(
        model.status_text,
        "Verified as Player.1234 (refreshing server settings)"
    );
    assert!(model.discord_delivery_visible);
    assert!(!model.guild_selection_available && !model.enable_toggle_available);
    assert!(!model.refresh_available && !model.disconnect_available);
    assert!(!model.discord_channel_selection_available && !model.discord_delivery_toggle_available);
    state.donbot.refreshing = false;
    state.donbot.state = DonBotConfigurationState::Verifying;
    state.donbot.account_name = None;
    let model = build_nexus_options_model(&state).donbot;
    assert!(model.verification_in_progress);
    assert!(!model.verify_available && !model.guild_selection_available);
    assert!(!model.enable_toggle_available && !model.disconnect_available);
    assert_eq!(model.status_text, "Verifying DonBot account");
}

#[test]
fn summary_and_aggregate_routes_are_independently_visible() {
    let mut state = connected();
    state.donbot.guilds[0].discord_delivery.defaults_available = false;
    assert!(
        build_nexus_options_model(&state)
            .donbot
            .discord_delivery_visible
    );
    state.donbot.guilds[0]
        .discord_delivery
        .channel_override_allowed = false;
    let model = build_nexus_options_model(&state).donbot;
    assert!(!model.discord_delivery_visible);
    assert!(model.discord_channel_selection_visible);
    assert_eq!(
        model.discord_delivery_status_text,
        "No authorized Discord log destination is available"
    );
    state.donbot.guilds[0]
        .discord_delivery
        .aggregate_defaults_available = false;
    assert!(
        !build_nexus_options_model(&state)
            .donbot
            .discord_channel_selection_visible
    );
    state.donbot.guilds[0]
        .discord_delivery
        .channel_override_allowed = true;
    state.donbot.guilds[0].discord_delivery.pve_summary = false;
    assert!(
        !build_nexus_options_model(&state)
            .donbot
            .discord_delivery_visible
    );
    for output in 0..4 {
        let policy = &mut state.donbot.guilds[0].discord_delivery;
        policy.pve_summary = output == 0;
        policy.wvw_summary = output == 1;
        policy.wvw_advanced = output == 2;
        policy.wvw_stream = output == 3;
        assert!(
            build_nexus_options_model(&state)
                .donbot
                .discord_delivery_visible
        );
    }
}

#[test]
fn unavailable_summary_reasons_and_unknown_guilds_are_explicit() {
    let mut state = connected();
    state.donbot.guilds[0].discord_delivery.pve_summary = false;
    assert_eq!(
        build_nexus_options_model(&state)
            .donbot
            .discord_delivery_status_text,
        "No Discord summary outputs are enabled in this server's DonBot settings"
    );
    state.donbot.guilds[0].discord_delivery.enabled = false;
    assert_eq!(
        build_nexus_options_model(&state)
            .donbot
            .discord_delivery_status_text,
        "Discord summaries are disabled in this server's DonBot settings"
    );
    state.donbot.discord_summary_delivery_v1 = false;
    assert_eq!(
        build_nexus_options_model(&state)
            .donbot
            .discord_delivery_status_text,
        "This DonBot deployment does not support Discord summaries"
    );
    state.donbot.selected_guild_id = "999".into();
    assert_eq!(
        build_nexus_options_model(&state)
            .donbot
            .discord_delivery_status_text,
        "Select a DonBot server to configure Discord summaries"
    );
    state.configuration.settings.donbot.enabled = false;
    assert!(
        !build_nexus_options_model(&state)
            .donbot
            .enable_toggle_available
    );
}

#[test]
fn storage_capability_and_wine_protection_are_honest() {
    let mut state = snapshot();
    state.configuration.persistent_secret_storage.mode =
        Some(SecretProtectionMode::WineCompatibility);
    let model = build_nexus_options_model(&state);
    assert!(model.protected_storage_text.contains("reduced protection"));
    assert!(model.donbot.verify_available && model.twitch.connect_available);
    for capability in [
        PersistentSecretStorageState::UnsupportedEnvironment,
        PersistentSecretStorageState::InitializationFailed,
    ] {
        state.configuration.persistent_secret_storage.state = capability;
        let model = build_nexus_options_model(&state);
        assert!(!model.donbot.verify_available && !model.twitch.connect_available);
        assert!(model.settings_editable);
        assert!(!model.protected_storage_text.contains("is available"));
    }
}

#[test]
fn all_mutations_stop_with_admission_configuration_or_workflow_shutdown() {
    for scenario in 0..3 {
        let mut state = connected();
        match scenario {
            0 => state.accepting_commands = false,
            1 => state.shutting_down = true,
            _ => state.configuration.shutting_down = true,
        }
        assert!(
            mutations(&build_nexus_options_model(&state))
                .iter()
                .all(|enabled| !enabled)
        );
    }
    let mut state = connected();
    state.donbot.shutting_down = true;
    state.twitch.shutting_down = true;
    let model = build_nexus_options_model(&state);
    assert!(model.settings_editable);
    assert!(mutations(&model)[2..].iter().all(|enabled| !enabled));
}

#[test]
fn authorization_model_never_contains_raw_code_or_uri() {
    let mut state = snapshot();
    state.twitch.state = TwitchConnectionState::AwaitingUser;
    state.twitch.user_code = Some("SYNTHETIC-USER-CODE".into());
    state.twitch.verification_uri =
        Some("https://www.twitch.tv/activate?code=SYNTHETIC-USER-CODE".into());
    let model = build_nexus_options_model(&state);
    assert_eq!(model.twitch.masked_user_code, Some("********"));
    assert!(model.twitch.authorization_available && model.twitch.disconnect_available);
    assert!(!format!("{model:?}").contains("SYNTHETIC-USER-CODE"));
    assert!(!format!("{model:?}").contains("https://www.twitch.tv"));
    state.twitch.user_code = Some("SHORT".into());
    assert_eq!(
        build_nexus_options_model(&state).twitch.masked_user_code,
        Some("********")
    );
    state.twitch.verification_uri = None;
    assert!(
        !build_nexus_options_model(&state)
            .twitch
            .authorization_available
    );
    assert_eq!(state.twitch.user_code.as_deref(), Some("SHORT"));
}

#[test]
fn twitch_state_matrix_and_test_message_gating() {
    use TwitchConnectionState::*;
    for connection in [
        Disconnected,
        Starting,
        AwaitingUser,
        Validating,
        Refreshing,
        Connected,
        Disconnecting,
        Error,
        ShuttingDown,
    ] {
        let mut state = snapshot();
        state.twitch.state = connection;
        let model = build_nexus_options_model(&state).twitch;
        assert_eq!(
            model.connect_available,
            matches!(connection, Disconnected | Error)
        );
        assert_eq!(model.enable_toggle_available, connection == Connected);
        assert_eq!(model.test_message_available, connection == Connected);
        assert_eq!(
            model.disconnect_available,
            !matches!(connection, Disconnected | Disconnecting | ShuttingDown)
        );
        assert!(!model.status_text.is_empty());
    }
    for test in [
        TwitchTestMessageState::Idle,
        TwitchTestMessageState::Sending,
        TwitchTestMessageState::Sent,
        TwitchTestMessageState::Error,
        TwitchTestMessageState::ShuttingDown,
    ] {
        let mut state = connected();
        state.twitch_test_message.state = test;
        state.twitch_test_message.diagnostic = Some(TwitchTestMessageError::Ambiguous);
        let model = build_nexus_options_model(&state).twitch;
        assert_eq!(
            model.test_message_available,
            !matches!(
                test,
                TwitchTestMessageState::Sending | TwitchTestMessageState::ShuttingDown
            )
        );
        assert!(
            model
                .test_message_diagnostic
                .unwrap()
                .contains("Automatic retry is disabled")
        );
    }
}

#[test]
fn failed_connections_allow_disabling_stale_posting() {
    let mut state = snapshot();
    state.configuration.settings.twitch.enabled = true;
    state.twitch.state = TwitchConnectionState::Error;
    state.twitch.diagnostic = Some(TwitchAuthenticationControllerError::ReconnectRequired);
    state.last_error = Some(NexusOptionsError::BrowserLaunchFailed);
    let model = build_nexus_options_model(&state);
    assert!(model.twitch.enable_toggle_available && model.twitch.connect_available);
    assert_eq!(
        model.twitch.diagnostic,
        Some("Reconnect Twitch to continue")
    );
    assert!(model.last_error.unwrap().contains("default browser"));
}

#[test]
fn stale_ordinary_drafts_preserve_all_destination_and_routing_state() {
    let mut current = snapshot().configuration.settings;
    let mut draft = NexusOrdinaryOptions::from(&current);
    draft.general.poll_interval_ms = 2500;
    draft.twitch_message_template = "Report: {url}".into();
    current.dps_report.detailed_wvw = true;
    current.wingman.enabled = false;
    current.donbot.enabled = true;
    current.donbot.selected_guild_id = "123".into();
    current.donbot.discord_delivery_enabled = true;
    current.donbot.discord_channel_override_explicit = true;
    current.donbot.selected_discord_channel_id = "456".into();
    current.twitch.enabled = true;
    let merged = draft
        .merge(&current, TwitchConnectionState::Connected)
        .unwrap();
    assert_eq!(merged.dps_report, current.dps_report);
    assert_eq!(merged.wingman, current.wingman);
    assert_eq!(merged.donbot, current.donbot);
    assert!(merged.twitch.enabled);
    assert_eq!(merged.general.poll_interval_ms, 2500);
    assert_eq!(merged.twitch.message_template, "Report: {url}");
    assert_eq!(current.general.poll_interval_ms, 1000);
}

#[test]
fn client_id_changes_require_disconnected_or_error_state() {
    use TwitchConnectionState::*;
    let current = snapshot().configuration.settings;
    let mut draft = NexusOrdinaryOptions::from(&current);
    draft.twitch_client_id = "newpublicclient".into();
    for connection in [
        Disconnected,
        Starting,
        AwaitingUser,
        Validating,
        Refreshing,
        Connected,
        Disconnecting,
        Error,
        ShuttingDown,
    ] {
        let result = draft.merge(&current, connection);
        if matches!(connection, Disconnected | Error) {
            assert_eq!(result.unwrap().twitch.client_id, "newpublicclient");
        } else {
            assert_eq!(result, Err(NexusOptionsError::ClientIdInUse));
        }
    }
}

#[test]
fn ordinary_validation_uses_current_posting_policy_and_safe_errors() {
    let mut current = snapshot().configuration.settings;
    let mut draft = NexusOrdinaryOptions::from(&current);
    draft.twitch_post_failure = false;
    draft.twitch_post_success = false;
    assert!(
        draft
            .merge(&current, TwitchConnectionState::Connected)
            .is_ok()
    );
    current.twitch.enabled = true;
    let Err(NexusOptionsError::InvalidSettings(errors)) =
        draft.merge(&current, TwitchConnectionState::Connected)
    else {
        panic!("expected validation failure")
    };
    assert!(
        errors
            .iter()
            .any(|error| error.code == SettingsErrorCode::TwitchPostingDisabled)
    );
    draft.general.log_directory = "SYNTHETIC-PRIVATE-PATH\n".into();
    let error = draft
        .merge(&current, TwitchConnectionState::Connected)
        .unwrap_err();
    assert!(!format!("{error:?}").contains("SYNTHETIC-PRIVATE-PATH"));
    assert!(!error.detail().contains("SYNTHETIC-PRIVATE-PATH"));
}

#[test]
fn imgui_labels_preserve_unicode_without_hidden_id_sequences() {
    for (input, expected) in [
        ("Guild ## Logs", "Guild # # Logs"),
        ("###", "# # #"),
        ("#####", "# # # # #"),
        ("Guild", "Guild"),
        ("\u{96ea}###\u{00e9}", "\u{96ea}# # #\u{00e9}"),
    ] {
        let escaped = escape_imgui_label_text(input);
        assert_eq!(escaped, expected);
        assert!(!escaped.contains("##"));
        assert_eq!(escape_imgui_label_text(&escaped), escaped);
    }
}
