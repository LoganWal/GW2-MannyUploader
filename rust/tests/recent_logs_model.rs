use manny_uploader::{application::*, domain::*, ui::*};
use std::{
    path::PathBuf,
    time::{Duration, Instant, SystemTime},
};

fn now() -> SystemTime {
    SystemTime::UNIX_EPOCH + Duration::from_secs(3 * 86400)
}
fn selection() -> SessionLogSelection {
    SessionLogSelection::new(now(), Instant::now())
}
fn job(id: u64) -> UploadJobSnapshot {
    UploadJobSnapshot {
        id: UploadJobId::new(id).unwrap(),
        record: UploadJobRecord {
            file: LogFileIdentity {
                canonical_path: PathBuf::from("synthetic-private-directory/log.zevtc"),
                size: 100,
                last_write_time: now(),
            },
            detected_at: now(),
            encounter_metadata: Some(EncounterMetadata {
                boss_id: 42,
                pov_account: "Player.1234".into(),
                remaining_health_basis_points: None,
            }),
            dps_report_result: Some(DpsReportResult {
                permalink: format!("https://dps.report/report{id}"),
                encounter_name: "Encounter".into(),
                boss_id: 42,
                mode: "CM".into(),
                success: true,
            }),
            wingman_upload_receipt: Some(WingmanUploadReceipt {
                permalink: format!("https://gw2wingman.nevermindcreations.de/log/{id}"),
            }),
            donbot_upload_receipt: Some(DonBotUploadReceipt {
                fight_log_id: Some(id + 100),
                ..Default::default()
            }),
            twitch_delivery_receipt: None,
            providers: std::array::from_fn(|_| ProviderStatus {
                state: ProviderState::Succeeded,
                attempts: 1,
                detail: "Completed".into(),
            }),
        },
    }
}
fn model(jobs: &[UploadJobSnapshot]) -> RecentLogsModel {
    build_recent_logs_model(jobs, &selection(), now(), ProviderSelection::default()).unwrap()
}
fn options() -> NexusOptionsSnapshot {
    let mut settings = Settings::new("synthetic".into());
    settings.donbot.enabled = true;
    settings.donbot.selected_guild_id = "123".into();
    NexusOptionsSnapshot {
        configuration: ConfigurationSnapshot {
            settings,
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
            state: DonBotConfigurationState::Verified,
            api_base_url: DEFAULT_DONBOT_API.into(),
            account_name: Some("Player.1234".into()),
            discord_summary_delivery_v1: true,
            discord_aggregate_delivery_v1: true,
            selected_guild_id: "123".into(),
            diagnostic: None,
            refreshing: false,
            revision: 1,
            shutting_down: false,
            guilds: vec![DonBotGuild {
                guild_id: "123".into(),
                guild_name: "Raid Guild".into(),
                discord_delivery: DonBotDiscordDeliveryPolicy {
                    enabled: true,
                    defaults_available: true,
                    channel_override_allowed: true,
                    pve_summary: true,
                    aggregate_enabled: true,
                    aggregate_defaults_available: true,
                    max_aggregate_fight_logs: 100,
                    channels: vec![DonBotChannel {
                        channel_id: "456".into(),
                        channel_name: "logs".into(),
                    }],
                    ..Default::default()
                },
            }],
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
fn delivery() -> DonBotAggregateDeliverySnapshot {
    DonBotAggregateDeliverySnapshot {
        state: DonBotAggregateDeliveryState::Idle,
        fight_log_count: 0,
        discord_delivery: None,
        diagnostic: None,
        credential_failure: None,
        revision: 1,
        shutting_down: false,
    }
}
fn select_all(recent: &RecentLogsModel) -> DonBotAggregateSelection {
    let mut selection = DonBotAggregateSelection::default();
    selection.reconcile(
        None,
        &recent.retained_job_ids,
        &recent
            .rows
            .iter()
            .map(RecentLogRow::aggregate_candidate)
            .collect::<Vec<_>>(),
    );
    selection
}

#[test]
fn visible_rows_reverse_coordinator_order_and_hidden_ids_stay_retained() {
    let mut older = job(1);
    older.record.file.last_write_time = now() - Duration::from_secs(1);
    let recent = model(&[older, job(2), job(3)]);
    assert_eq!(
        recent
            .retained_job_ids
            .iter()
            .map(|id| id.get())
            .collect::<Vec<_>>(),
        [1, 2, 3]
    );
    assert_eq!(
        recent
            .rows
            .iter()
            .map(|row| row.id.get())
            .collect::<Vec<_>>(),
        [3, 2]
    );
    assert_eq!(
        recent.dps_report_clipboard_text,
        "https://dps.report/report3\nhttps://dps.report/report2"
    );
    assert_eq!(recent.dps_report_url_count, 2);
    assert_eq!(
        recent.donbot_aggregate_url,
        "https://donbot.walmslo.com/logs/aggregate?ids=103,102"
    );
    assert_eq!(recent.donbot_fight_count, 2);
    assert!(!format!("{recent:?}").contains("synthetic-private-directory"));
}

#[test]
fn selection_switch_and_rolling_cutoff_only_change_presentation() {
    let mut old = job(1);
    old.record.file.last_write_time = now() - Duration::from_secs(86400);
    let jobs = [old, job(2)];
    let before = jobs.clone();
    let selection = selection();
    let rolling = selection
        .propose(LogSelectionMode::Last24Hours, now(), Instant::now())
        .unwrap()
        .unwrap();
    assert_eq!(
        build_recent_logs_model(&jobs, &rolling, now(), ProviderSelection::default())
            .unwrap()
            .rows
            .len(),
        2
    );
    let later = build_recent_logs_model(
        &jobs,
        &rolling,
        now() + Duration::from_secs(1),
        ProviderSelection::default(),
    )
    .unwrap();
    assert_eq!(later.rows.len(), 1);
    assert_eq!(later.retained_job_ids.len(), 2);
    assert_eq!(jobs, before);
}

#[test]
fn provider_visibility_is_live_and_preserves_settled_rows() {
    let jobs = [job(1)];
    let off = model(&jobs);
    let enabled = ProviderSelection {
        dps_report: true,
        wingman: true,
        donbot: true,
        twitch: true,
    };
    let on = build_recent_logs_model(&jobs, &selection(), now(), enabled).unwrap();
    assert_eq!(off.rows, on.rows);
    assert_eq!(on.enabled_providers, enabled);
    assert!(
        on.rows[0]
            .providers
            .iter()
            .all(|provider| provider.state == ProviderState::Succeeded)
    );
}

#[test]
fn hostile_or_missing_receipts_never_become_links_or_selection_candidates() {
    for bad in [
        "https://evil.invalid/report",
        "https://dps.report/",
        "https://dps.report/\"bad",
        "https://wvw.report/report",
    ] {
        let mut current = job(1);
        current.record.dps_report_result.as_mut().unwrap().permalink = bad.into();
        current
            .record
            .wingman_upload_receipt
            .as_mut()
            .unwrap()
            .permalink = "https://evil.invalid/log".into();
        current
            .record
            .donbot_upload_receipt
            .as_mut()
            .unwrap()
            .fight_log_id = Some(0);
        let row = build_recent_log_row(&current);
        assert!(row.dps_report_url.is_none());
        assert!(!row.rechat_available);
        assert!(!row.wingman_report_available && !row.aggregate_candidate().eligible);
        let model = model(&[current]);
        assert!(model.dps_report_clipboard_text.is_empty());
        assert!(model.donbot_aggregate_url.is_empty());
    }
    let mut wvw = job(1);
    let report = wvw.record.dps_report_result.as_mut().unwrap();
    report.boss_id = 1;
    report.permalink = "https://wvw.report/report".into();
    assert!(build_recent_log_row(&wvw).dps_report_url.is_some());
    for id in [i64::MAX as u64 + 1, u64::MAX] {
        wvw.record
            .donbot_upload_receipt
            .as_mut()
            .unwrap()
            .fight_log_id = Some(id);
        assert!(!build_recent_log_row(&wvw).aggregate_candidate().eligible);
    }
}

#[test]
fn encounter_labels_follow_metadata_result_and_failure_health() {
    let mut current = job(1);
    assert_eq!(build_recent_log_row(&current).encounter, "Encounter (CM)");
    current.record.dps_report_result.as_mut().unwrap().success = false;
    for (health, text) in [
        (0, "0"),
        (1, "0.01"),
        (10, "0.1"),
        (100, "1"),
        (1250, "12.5"),
        (1234, "12.34"),
        (10000, "100"),
    ] {
        current
            .record
            .encounter_metadata
            .as_mut()
            .unwrap()
            .remaining_health_basis_points = Some(health);
        assert_eq!(
            build_recent_log_row(&current).encounter,
            format!("Encounter (CM) ({text}% left)")
        );
        assert_eq!(
            build_recent_log_row(&current).encounter_success,
            Some(false)
        );
    }
    current
        .record
        .encounter_metadata
        .as_mut()
        .unwrap()
        .remaining_health_basis_points = Some(10001);
    assert_eq!(build_recent_log_row(&current).encounter, "Encounter (CM)");
    current.record.dps_report_result = None;
    assert_eq!(build_recent_log_row(&current).encounter, "Encounter 42");
    current.record.encounter_metadata = None;
    assert_eq!(build_recent_log_row(&current).encounter, "Reading metadata");
}

#[test]
fn retry_and_replay_availability_obeys_state_and_metadata() {
    use ProviderState::*;
    for state in [
        Disabled,
        Waiting,
        Active,
        Succeeded,
        Skipped,
        RetryScheduled(Instant::now()),
        Failed,
        Cancelled,
    ] {
        let mut current = job(1);
        for provider in &mut current.record.providers {
            provider.state = state;
        }
        let row = build_recent_log_row(&current);
        assert_eq!(row.reupload_available, !state.is_busy());
        assert_eq!(row.rechat_available, !state.is_busy());
        assert!(
            row.providers
                .iter()
                .all(|provider| provider.retry_available == (state == Failed))
        );
        assert!(
            row.providers
                .iter()
                .all(|provider| !provider.status.is_empty() && provider.detail == "Completed")
        );
        current.record.encounter_metadata = None;
        let row = build_recent_log_row(&current);
        assert!(!row.reupload_available);
        assert!(
            row.providers
                .iter()
                .all(|provider| !provider.retry_available)
        );
    }
    let mut current = job(1);
    current.record.providers[3].state = Active;
    assert!(build_recent_log_row(&current).reupload_available);
    current.record.providers[3].state = Succeeded;
    current.record.providers[0].state = Active;
    assert!(build_recent_log_row(&current).rechat_available);
    current.record.file.canonical_path = PathBuf::from("single.zevtc");
    assert!(!build_recent_log_row(&current).directory_available);
}

#[test]
fn input_limits_and_duplicate_jobs_fail_before_presentation() {
    assert_eq!(
        build_recent_logs_model(
            &[job(1), job(1)],
            &selection(),
            now(),
            ProviderSelection::default()
        ),
        Err(RecentLogsModelError::DuplicateJob)
    );
    assert_eq!(
        build_recent_logs_model(
            &vec![job(1); MAX_HISTORY_LIMIT + 1],
            &selection(),
            now(),
            ProviderSelection::default()
        ),
        Err(RecentLogsModelError::TooManyLogs)
    );
    let empty = model(&[]);
    assert!(
        empty.rows.is_empty()
            && empty.dps_report_clipboard_text.is_empty()
            && empty.donbot_aggregate_url.is_empty()
    );
}

#[test]
fn utc_dates_cover_leap_years_centuries_negative_fractions_and_bounds() {
    let cases: [(i64, &str); 13] = [
        (0, "1970-01-01 00:00:00 UTC"),
        (-1, "1969-12-31 23:59:59 UTC"),
        (951782400, "2000-02-29 00:00:00 UTC"),
        (4107542400, "2100-03-01 00:00:00 UTC"),
        (-2203891200, "1900-03-01 00:00:00 UTC"),
        (1709164800, "2024-02-29 00:00:00 UTC"),
        (-11676096000, "1600-01-01 00:00:00 UTC"),
        (-11644473600, "1601-01-01 00:00:00 UTC"),
        (-62135596800, "0001-01-01 00:00:00 UTC"),
        (-62135596801, "Time unavailable"),
        (253402300799, "9999-12-31 23:59:59 UTC"),
        (253402300800, "Time unavailable"),
        (3661, "1970-01-01 01:01:01 UTC"),
    ];
    for (seconds, text) in cases {
        let time = if seconds < 0 {
            SystemTime::UNIX_EPOCH.checked_sub(Duration::from_secs(seconds.unsigned_abs()))
        } else {
            SystemTime::UNIX_EPOCH.checked_add(Duration::from_secs(seconds as u64))
        };
        let Some(time) = time else {
            // Windows SystemTime starts in 1601. Earlier calendar cases also run inside the
            // formatter's unit test without constructing an unrepresentable native timestamp.
            assert!(cfg!(windows) && seconds < -11_644_473_600);
            continue;
        };
        assert_eq!(format_detected_at(time), text, "{seconds}");
    }
    assert_eq!(
        format_detected_at(SystemTime::UNIX_EPOCH - Duration::from_nanos(100)),
        "1969-12-31 23:59:59 UTC"
    );
    assert_eq!(
        format_detected_at(SystemTime::UNIX_EPOCH + Duration::from_nanos(999999999)),
        "1970-01-01 00:00:00 UTC"
    );
}

#[test]
fn aggregate_uses_selected_visible_rows_and_preserves_intent_during_unavailability() {
    let recent = model(&[job(1), job(2), job(3)]);
    let mut selection = select_all(&recent);
    selection.set_selected(UploadJobId::new(2).unwrap(), false);
    let mut options = options();
    let delivery = delivery();
    let aggregate = build_donbot_aggregate_model(&recent, &selection, &options, &delivery);
    assert!(aggregate.send_available);
    assert_eq!(
        aggregate
            .selected_jobs
            .iter()
            .map(|id| id.get())
            .collect::<Vec<_>>(),
        [3, 1]
    );
    assert_eq!(
        aggregate.selected_aggregate_url,
        "https://donbot.walmslo.com/logs/aggregate?ids=103,101"
    );
    options.configuration.settings.donbot.enabled = false;
    let disabled = build_donbot_aggregate_model(&recent, &selection, &options, &delivery);
    assert!(!disabled.send_available);
    assert_eq!(disabled.selected_jobs, aggregate.selected_jobs);
    assert!(
        recent
            .rows
            .iter()
            .all(|row| row.aggregate_candidate().eligible)
    );
    options.configuration.settings.donbot.enabled = true;
    assert!(build_donbot_aggregate_model(&recent, &selection, &options, &delivery).send_available);
}

#[test]
fn aggregate_count_duplicates_busy_refresh_and_shutdown_gate_only_send() {
    let recent = model(&[job(1), job(2), job(3)]);
    let selection = select_all(&recent);
    for gate in 0..10 {
        let mut options = options();
        let mut delivery = delivery();
        match gate {
            0 => {
                options.donbot.guilds[0]
                    .discord_delivery
                    .max_aggregate_fight_logs = 2
            }
            1 => options.donbot.refreshing = true,
            2 => options.donbot.discord_aggregate_delivery_v1 = false,
            3 => options.donbot.guilds[0].discord_delivery.aggregate_enabled = false,
            4 => {
                options.donbot.guilds[0]
                    .discord_delivery
                    .aggregate_defaults_available = false
            }
            5 => options.accepting_commands = false,
            6 => options.configuration.shutting_down = true,
            7 => delivery.state = DonBotAggregateDeliveryState::Queued,
            8 => delivery.state = DonBotAggregateDeliveryState::Sending,
            _ => delivery.shutting_down = true,
        }
        let model = build_donbot_aggregate_model(&recent, &selection, &options, &delivery);
        assert!(!model.send_available, "gate {gate}");
        assert_eq!(model.selected_jobs.len(), 3);
        assert!(!model.selected_aggregate_url.is_empty());
    }
    let mut duplicate = job(2);
    duplicate
        .record
        .donbot_upload_receipt
        .as_mut()
        .unwrap()
        .fight_log_id = Some(101);
    let duplicated = model(&[job(1), duplicate]);
    let selected = select_all(&duplicated);
    assert!(
        !build_donbot_aggregate_model(&duplicated, &selected, &options(), &delivery())
            .send_available
    );
    let one = model(&[job(1)]);
    let selected = select_all(&one);
    assert!(!build_donbot_aggregate_model(&one, &selected, &options(), &delivery()).send_available);
}

#[test]
fn aggregate_client_limit_and_channel_override_follow_authorized_route() {
    let recent = model(&(1..=101).map(job).collect::<Vec<_>>());
    let mut selection = select_all(&recent);
    let mut options = options();
    options.donbot.guilds[0]
        .discord_delivery
        .max_aggregate_fight_logs = 200;
    let aggregate = build_donbot_aggregate_model(&recent, &selection, &options, &delivery());
    assert_eq!(aggregate.maximum_fights, 100);
    assert!(!aggregate.send_available);
    selection.set_selected(UploadJobId::new(1).unwrap(), false);
    options.donbot.guilds[0]
        .discord_delivery
        .aggregate_defaults_available = false;
    options
        .configuration
        .settings
        .donbot
        .discord_channel_override_explicit = true;
    options
        .configuration
        .settings
        .donbot
        .selected_discord_channel_id = "456".into();
    assert!(
        build_donbot_aggregate_model(&recent, &selection, &options, &delivery()).send_available
    );
    options
        .configuration
        .settings
        .donbot
        .selected_discord_channel_id = "999".into();
    assert!(
        !build_donbot_aggregate_model(&recent, &selection, &options, &delivery()).send_available
    );
}

#[test]
fn quick_access_all_destination_combinations_and_twitch_priority() {
    let options = options();
    for mask in 0..16 {
        let mut settings = options.configuration.settings.clone();
        settings.dps_report.enabled = mask & 1 != 0;
        settings.wingman.enabled = mask & 2 != 0;
        settings.donbot.enabled = mask & 4 != 0;
        settings.twitch.enabled = mask & 8 != 0;
        let status = build_quick_access_status(&settings, &options.donbot);
        assert_eq!(
            status.tint,
            if mask & 8 != 0 {
                QuickAccessTint::Twitch
            } else if mask != 0 {
                QuickAccessTint::Upload
            } else {
                QuickAccessTint::Idle
            }
        );
        assert!(status.tooltip.starts_with("View MannyUploader list"));
        assert_eq!(
            status.tooltip.lines().count(),
            1 + (mask as u32).count_ones() as usize
        );
        for (bit, label) in [
            (1, "dps.report"),
            (2, "GW2Wingman"),
            (4, "DonBot"),
            (8, "Twitch chat"),
        ] {
            assert_eq!(status.tooltip.contains(label), mask & bit != 0);
        }
    }
}

#[test]
fn quick_access_routes_fallbacks_and_unicode_names() {
    let mut options = options();
    options
        .configuration
        .settings
        .donbot
        .discord_delivery_enabled = true;
    let status = build_quick_access_status(&options.configuration.settings, &options.donbot);
    assert_eq!(
        status.tooltip,
        "View MannyUploader list\nUploading to dps.report\nUploading to GW2Wingman\nUploading to DonBot - Raid Guild\nPosting DonBot summaries - guild defaults"
    );
    options
        .configuration
        .settings
        .donbot
        .discord_channel_override_explicit = true;
    options
        .configuration
        .settings
        .donbot
        .selected_discord_channel_id = "456".into();
    options.donbot.guilds[0].guild_name = "\u{96ea}\nGuild".into();
    options.donbot.guilds[0].discord_delivery.channels[0].channel_name = "\u{00e9} logs".into();
    let status = build_quick_access_status(&options.configuration.settings, &options.donbot);
    assert!(status.tooltip.contains("\u{96ea} Guild"));
    assert!(status.tooltip.ends_with("#\u{00e9} logs"));
    assert_eq!(status.tooltip.lines().count(), 5);
    options.donbot.guilds.clear();
    let status = build_quick_access_status(&options.configuration.settings, &options.donbot);
    assert!(status.tooltip.contains("DonBot - 123"));
    assert!(status.tooltip.ends_with("selected channel"));
    options.configuration.settings.donbot.enabled = false;
    assert!(
        !build_quick_access_status(&options.configuration.settings, &options.donbot)
            .tooltip
            .contains("summaries")
    );
}
