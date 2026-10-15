use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

use manny_uploader::domain::*;

fn file() -> LogFileIdentity {
    LogFileIdentity {
        canonical_path: PathBuf::from("logs/example.zevtc"),
        size: 1234,
        last_write_time: SystemTime::UNIX_EPOCH,
    }
}

fn job() -> UploadJob {
    UploadJob::new(
        UploadJobId::new(42).unwrap(),
        file(),
        SystemTime::UNIX_EPOCH,
        ProviderSelection {
            dps_report: true,
            wingman: true,
            donbot: true,
            twitch: true,
        },
    )
    .unwrap()
}

fn transition(
    job: &mut UploadJob,
    provider: Provider,
    next: ProviderState,
) -> Result<(), JobError> {
    job.transition(provider, next, String::new())
}

fn report() -> DpsReportResult {
    DpsReportResult {
        permalink: "https://dps.report/example".into(),
        encounter_name: "Example".into(),
        boss_id: 123,
        mode: "CM".into(),
        success: true,
    }
}

fn complete_report(job: &mut UploadJob) {
    transition(job, Provider::DpsReport, ProviderState::Active).unwrap();
    job.complete_dps_report(report(), "uploaded".into())
        .unwrap();
}

fn metadata() -> EncounterMetadata {
    EncounterMetadata {
        boss_id: 123,
        pov_account: "Player.1234".into(),
        remaining_health_basis_points: Some(100),
    }
}

fn sent_receipt() -> TwitchDeliveryReceipt {
    TwitchDeliveryReceipt {
        status: TwitchDeliveryStatus::Sent,
        message_id: Some("message-123".into()),
    }
}

#[test]
fn creation_validates_identity_and_selection() {
    assert_eq!(UploadJobId::new(0), Err(JobError::InvalidJobId));
    let mut empty = file();
    empty.canonical_path.clear();
    assert!(matches!(
        UploadJob::new(
            UploadJobId::new(1).unwrap(),
            empty,
            SystemTime::UNIX_EPOCH,
            ProviderSelection::default()
        ),
        Err(JobError::EmptyPath)
    ));
    assert!(matches!(
        UploadJob::new(
            UploadJobId::new(1).unwrap(),
            file(),
            SystemTime::UNIX_EPOCH,
            ProviderSelection {
                twitch: true,
                ..ProviderSelection::default()
            }
        ),
        Err(JobError::TwitchRequiresDpsReport)
    ));
    let selected = UploadJob::new(
        UploadJobId::new(42).unwrap(),
        file(),
        SystemTime::UNIX_EPOCH,
        ProviderSelection {
            wingman: true,
            donbot: true,
            ..ProviderSelection::default()
        },
    )
    .unwrap();
    assert_eq!(selected.id().get(), 42);
    for provider in Provider::ALL {
        let expected = if matches!(provider, Provider::Wingman | Provider::DonBot) {
            ProviderState::Waiting
        } else {
            ProviderState::Disabled
        };
        assert_eq!(selected.provider_status(provider).state, expected);
        assert_eq!(selected.provider_status(provider).attempts, 0);
    }
}

#[test]
fn every_state_pair_obeys_the_transition_contract() {
    let deadline = Instant::now();
    let states = [
        ProviderState::Disabled,
        ProviderState::Waiting,
        ProviderState::Active,
        ProviderState::Succeeded,
        ProviderState::Skipped,
        ProviderState::RetryScheduled(deadline),
        ProviderState::Failed,
        ProviderState::Cancelled,
    ];
    // Rows and columns use the state ordering above. This table is the acceptance specification.
    let allowed = [
        [false, false, false, false, false, false, false, false],
        [false, false, true, false, true, false, true, true],
        [false, false, false, true, false, true, true, true],
        [false, false, false, false, false, false, false, false],
        [false, false, false, false, false, false, false, false],
        [false, false, true, false, false, false, true, true],
        [false, true, false, false, false, false, false, true],
        [false, false, false, false, false, false, false, false],
    ];
    for (row, &current) in states.iter().enumerate() {
        for (column, &next) in states.iter().enumerate() {
            let mut subject = job();
            match current {
                ProviderState::Disabled => {
                    subject = UploadJob::new(
                        UploadJobId::new(1).unwrap(),
                        file(),
                        SystemTime::UNIX_EPOCH,
                        ProviderSelection::default(),
                    )
                    .unwrap();
                }
                ProviderState::Waiting => {}
                ProviderState::Active => {
                    transition(&mut subject, Provider::Wingman, current).unwrap()
                }
                ProviderState::Succeeded | ProviderState::RetryScheduled(_) => {
                    transition(&mut subject, Provider::Wingman, ProviderState::Active).unwrap();
                    transition(&mut subject, Provider::Wingman, current).unwrap();
                }
                _ => transition(&mut subject, Provider::Wingman, current).unwrap(),
            }
            let before = subject.snapshot().clone();
            let result = transition(&mut subject, Provider::Wingman, next);
            assert_eq!(
                result.is_ok(),
                allowed[row][column],
                "{current:?} -> {next:?}"
            );
            if result.is_err() {
                assert_eq!(subject.snapshot(), &before);
            }
        }
    }
}

#[test]
fn retries_preserve_attempt_counts_and_clear_deadlines() {
    let mut subject = job();
    transition(&mut subject, Provider::DpsReport, ProviderState::Active).unwrap();
    let deadline = Instant::now() + Duration::from_secs(30);
    transition(
        &mut subject,
        Provider::DpsReport,
        ProviderState::RetryScheduled(deadline),
    )
    .unwrap();
    assert_eq!(
        subject.provider_status(Provider::DpsReport).state,
        ProviderState::RetryScheduled(deadline)
    );
    assert_eq!(subject.provider_status(Provider::DpsReport).attempts, 1);
    transition(&mut subject, Provider::DpsReport, ProviderState::Active).unwrap();
    assert_eq!(
        subject.provider_status(Provider::DpsReport).state,
        ProviderState::Active
    );
    assert_eq!(subject.provider_status(Provider::DpsReport).attempts, 2);
}

#[test]
fn attempt_overflow_is_rejected_without_mutation() {
    let mut record = job().snapshot().clone();
    record.providers[Provider::Wingman as usize].attempts = u32::MAX;
    let mut subject = UploadJob::restore(UploadJobId::new(1).unwrap(), record).unwrap();
    subject.prepare_manual_retry(Provider::Wingman).unwrap();
    let before = subject.snapshot().clone();
    assert_eq!(
        transition(&mut subject, Provider::Wingman, ProviderState::Active),
        Err(JobError::AttemptLimitReached)
    );
    assert_eq!(subject.snapshot(), &before);
}

#[test]
fn dps_completion_and_twitch_require_valid_results() {
    let mut subject = job();
    assert_eq!(
        transition(&mut subject, Provider::Twitch, ProviderState::Active),
        Err(JobError::DpsReportResultRequired)
    );
    assert_eq!(
        subject.complete_dps_report(report(), String::new()),
        Err(JobError::InvalidTransition)
    );
    transition(&mut subject, Provider::DpsReport, ProviderState::Active).unwrap();
    assert_eq!(
        transition(&mut subject, Provider::DpsReport, ProviderState::Succeeded),
        Err(JobError::DpsReportResultRequired)
    );
    let mut empty = report();
    empty.permalink.clear();
    assert_eq!(
        subject.complete_dps_report(empty, String::new()),
        Err(JobError::EmptyPermalink)
    );
    assert!(subject.snapshot().dps_report_result.is_none());
    subject
        .complete_dps_report(report(), String::new())
        .unwrap();
    transition(&mut subject, Provider::Twitch, ProviderState::Active).unwrap();
    assert_eq!(
        transition(&mut subject, Provider::Twitch, ProviderState::Succeeded),
        Err(JobError::InvalidTwitchDelivery)
    );
    subject.record_twitch_delivery(sent_receipt()).unwrap();
    assert_eq!(
        subject.record_twitch_delivery(sent_receipt()),
        Err(JobError::TwitchDeliveryAlreadyRecorded)
    );
    transition(&mut subject, Provider::Twitch, ProviderState::Succeeded).unwrap();
}

#[test]
fn twitch_receipt_validation_rejects_missing_invalid_and_unexpected_ids() {
    for id in [
        None,
        Some(String::new()),
        Some("has space".into()),
        Some("line\nbreak".into()),
        Some("x".repeat(257)),
        Some("\u{e9}".into()),
    ] {
        let mut subject = job();
        complete_report(&mut subject);
        transition(&mut subject, Provider::Twitch, ProviderState::Active).unwrap();
        assert_eq!(
            subject.record_twitch_delivery(TwitchDeliveryReceipt {
                status: TwitchDeliveryStatus::Sent,
                message_id: id
            }),
            Err(JobError::InvalidTwitchDelivery)
        );
        assert!(subject.snapshot().twitch_delivery_receipt.is_none());
    }
    for id in [String::new(), "message".into(), "invalid id".into()] {
        let mut subject = job();
        complete_report(&mut subject);
        transition(&mut subject, Provider::Twitch, ProviderState::Active).unwrap();
        assert_eq!(
            subject.record_twitch_delivery(TwitchDeliveryReceipt {
                status: TwitchDeliveryStatus::AutoMod,
                message_id: Some(id)
            }),
            Err(JobError::InvalidTwitchDelivery)
        );
    }
    let mut subject = job();
    assert_eq!(
        subject.record_twitch_delivery(sent_receipt()),
        Err(JobError::InvalidTransition)
    );
    complete_report(&mut subject);
    transition(&mut subject, Provider::Twitch, ProviderState::Active).unwrap();
    subject
        .record_twitch_delivery(TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::Sent,
            message_id: Some("x".repeat(256)),
        })
        .unwrap();
}

#[test]
fn metadata_is_assigned_once_and_snapshots_are_independent() {
    let mut subject = job();
    let earlier = subject.snapshot().clone();
    subject.set_encounter_metadata(metadata()).unwrap();
    assert_eq!(
        subject.set_encounter_metadata(metadata()),
        Err(JobError::MetadataAlreadySet)
    );
    assert_eq!(subject.snapshot().encounter_metadata, Some(metadata()));
    assert!(earlier.encounter_metadata.is_none());
}

#[test]
fn wingman_receipt_requires_active_nonempty_and_single_assignment() {
    let mut subject = job();
    let receipt = WingmanUploadReceipt {
        permalink: "https://gw2wingman.nevermindcreations.de/log/example".into(),
    };
    assert_eq!(
        subject.record_wingman_upload(receipt.clone()),
        Err(JobError::InvalidTransition)
    );
    transition(&mut subject, Provider::Wingman, ProviderState::Active).unwrap();
    assert_eq!(
        subject.record_wingman_upload(WingmanUploadReceipt {
            permalink: String::new()
        }),
        Err(JobError::InvalidWingmanUpload)
    );
    subject.record_wingman_upload(receipt.clone()).unwrap();
    assert_eq!(
        subject.record_wingman_upload(receipt),
        Err(JobError::WingmanUploadAlreadyRecorded)
    );
}

#[test]
fn donbot_receipts_bound_ids_and_normalized_delivery_counts() {
    let invalid = [
        DonBotUploadReceipt {
            upload_id: Some(0),
            ..Default::default()
        },
        DonBotUploadReceipt {
            fight_log_id: Some(0),
            ..Default::default()
        },
        DonBotUploadReceipt {
            fight_log_id: Some(i64::MAX as u64 + 1),
            ..Default::default()
        },
        DonBotUploadReceipt {
            discord_delivery: DonBotDiscordDeliveryReceipt {
                sent: 1,
                ..Default::default()
            },
            ..Default::default()
        },
        DonBotUploadReceipt {
            discord_delivery: DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: 2,
                ..Default::default()
            },
            ..Default::default()
        },
        DonBotUploadReceipt {
            discord_delivery: DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Sent,
                sent: 5,
                ..Default::default()
            },
            ..Default::default()
        },
        DonBotUploadReceipt {
            discord_delivery: DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: u16::MAX,
                failed: u16::MAX,
                ..Default::default()
            },
            ..Default::default()
        },
    ];
    for receipt in invalid {
        let mut subject = job();
        transition(&mut subject, Provider::DonBot, ProviderState::Active).unwrap();
        assert_eq!(
            subject.record_donbot_upload(receipt),
            Err(JobError::InvalidDonBotUpload)
        );
        assert!(subject.snapshot().donbot_upload_receipt.is_none());
    }
    for guild in [
        "",
        "0",
        "01",
        "+1",
        "-1",
        "abc",
        "9223372036854775808",
        "12345678901234567890",
    ] {
        let mut subject = job();
        transition(&mut subject, Provider::DonBot, ProviderState::Active).unwrap();
        assert_eq!(
            subject.record_donbot_upload(DonBotUploadReceipt {
                guild_id: Some(guild.into()),
                ..Default::default()
            }),
            Err(JobError::InvalidDonBotUpload)
        );
    }
    let mut subject = job();
    let receipt = DonBotUploadReceipt {
        upload_id: Some(u64::MAX),
        fight_log_id: Some(i64::MAX as u64),
        guild_id: Some(i64::MAX.to_string()),
        discord_delivery: DonBotDiscordDeliveryReceipt {
            outcome: DonBotDiscordDeliveryOutcome::Partial,
            sent: 2,
            skipped: 1,
            ..Default::default()
        },
    };
    assert_eq!(
        subject.record_donbot_upload(receipt.clone()),
        Err(JobError::InvalidTransition)
    );
    transition(&mut subject, Provider::DonBot, ProviderState::Active).unwrap();
    subject.record_donbot_upload(receipt.clone()).unwrap();
    assert_eq!(
        subject.record_donbot_upload(receipt),
        Err(JobError::DonBotUploadAlreadyRecorded)
    );
}

#[test]
fn manual_retry_rearms_only_failed_provider_and_skipped_dependencies() {
    let mut subject = job();
    assert_eq!(
        subject.prepare_manual_retry(Provider::DpsReport),
        Err(JobError::ManualRetryRequiresFailure)
    );
    transition(&mut subject, Provider::DpsReport, ProviderState::Failed).unwrap();
    transition(&mut subject, Provider::Wingman, ProviderState::Skipped).unwrap();
    transition(&mut subject, Provider::DonBot, ProviderState::Cancelled).unwrap();
    transition(&mut subject, Provider::Twitch, ProviderState::Skipped).unwrap();
    subject.prepare_manual_retry(Provider::DpsReport).unwrap();
    assert_eq!(
        subject.provider_status(Provider::Wingman).state,
        ProviderState::Waiting
    );
    assert_eq!(
        subject.provider_status(Provider::Twitch).state,
        ProviderState::Waiting
    );
    assert_eq!(
        subject.provider_status(Provider::DonBot).state,
        ProviderState::Cancelled
    );
    complete_report(&mut subject);
    transition(&mut subject, Provider::Twitch, ProviderState::Active).unwrap();
    subject
        .record_twitch_delivery(TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::AutoMod,
            message_id: None,
        })
        .unwrap();
    assert_eq!(
        transition(&mut subject, Provider::Twitch, ProviderState::Succeeded),
        Err(JobError::InvalidTwitchDelivery)
    );
    subject
        .transition(Provider::Twitch, ProviderState::Failed, "held".into())
        .unwrap();
    subject.prepare_manual_retry(Provider::Twitch).unwrap();
    assert!(subject.snapshot().twitch_delivery_receipt.is_none());
    assert!(subject.provider_status(Provider::Twitch).detail.is_empty());
    assert_eq!(subject.provider_status(Provider::Twitch).attempts, 1);
}

#[test]
fn restore_normalizes_interrupted_work_and_preserves_settled_receipts() {
    let states = [
        ProviderState::Disabled,
        ProviderState::Waiting,
        ProviderState::Active,
        ProviderState::Succeeded,
        ProviderState::Skipped,
        ProviderState::RetryScheduled(Instant::now()),
        ProviderState::Failed,
        ProviderState::Cancelled,
    ];
    for state in states {
        let mut record = job().snapshot().clone();
        record.file.canonical_path = PathBuf::from("logs/\u{65e5}\u{672c}\u{8a9e}/example.zevtc");
        record.dps_report_result = Some(report());
        record.twitch_delivery_receipt = Some(sent_receipt());
        record.providers[0] = ProviderStatus {
            state,
            attempts: 3,
            detail: "safe detail".into(),
        };
        let restored = UploadJob::restore(UploadJobId::new(99).unwrap(), record.clone()).unwrap();
        let status = restored.provider_status(Provider::DpsReport);
        assert_eq!(
            status.state,
            if state.is_busy() {
                ProviderState::Failed
            } else {
                state
            }
        );
        assert_eq!(status.attempts, 3);
        assert_eq!(
            status.detail,
            if state.is_busy() {
                "Interrupted by the previous game session"
            } else {
                "safe detail"
            }
        );
        assert_eq!(restored.snapshot().file, record.file);
        assert_eq!(
            restored.snapshot().dps_report_result,
            record.dps_report_result
        );
        assert_eq!(
            restored.snapshot().twitch_delivery_receipt,
            record.twitch_delivery_receipt
        );
        assert!(
            restored
                .snapshot()
                .providers
                .iter()
                .all(|status| !status.state.is_busy())
        );
    }
}

#[test]
fn restore_rejects_invalid_paths_and_receipts() {
    let base = job().snapshot().clone();
    let mut cases = Vec::new();
    let mut record = base.clone();
    record.file.canonical_path.clear();
    cases.push((record, JobError::EmptyPath));
    let mut record = base.clone();
    record.dps_report_result = Some(DpsReportResult {
        permalink: String::new(),
        ..report()
    });
    cases.push((record, JobError::EmptyPermalink));
    let mut record = base.clone();
    record.wingman_upload_receipt = Some(WingmanUploadReceipt {
        permalink: String::new(),
    });
    cases.push((record, JobError::InvalidWingmanUpload));
    let mut record = base.clone();
    record.donbot_upload_receipt = Some(DonBotUploadReceipt {
        upload_id: Some(0),
        ..Default::default()
    });
    cases.push((record, JobError::InvalidDonBotUpload));
    let mut record = base;
    record.twitch_delivery_receipt = Some(TwitchDeliveryReceipt {
        status: TwitchDeliveryStatus::Sent,
        message_id: None,
    });
    cases.push((record, JobError::InvalidTwitchDelivery));
    for (record, error) in cases {
        assert_eq!(
            UploadJob::restore(UploadJobId::new(1).unwrap(), record).err(),
            Some(error)
        );
    }
}

#[test]
fn explicit_replay_requires_metadata_idle_state_and_retained_report_for_chat() {
    let mut subject = job();
    assert_eq!(
        subject.prepare_explicit_delivery(Provider::Twitch),
        Err(JobError::EncounterMetadataRequired)
    );
    subject.set_encounter_metadata(metadata()).unwrap();
    assert_eq!(
        subject.prepare_explicit_delivery(Provider::Twitch),
        Err(JobError::ExplicitDeliveryBusy)
    );
    transition(&mut subject, Provider::Twitch, ProviderState::Cancelled).unwrap();
    assert_eq!(
        subject.prepare_explicit_delivery(Provider::Twitch),
        Err(JobError::DpsReportResultRequired)
    );
    complete_report(&mut subject);
    subject.prepare_explicit_delivery(Provider::Twitch).unwrap();
    transition(&mut subject, Provider::Twitch, ProviderState::Active).unwrap();
    subject.record_twitch_delivery(sent_receipt()).unwrap();
    transition(&mut subject, Provider::Twitch, ProviderState::Succeeded).unwrap();
    assert_eq!(
        subject.prepare_manual_retry(Provider::Twitch),
        Err(JobError::ManualRetryRequiresFailure)
    );
    subject.prepare_explicit_delivery(Provider::Twitch).unwrap();
    assert!(subject.snapshot().twitch_delivery_receipt.is_none());
    assert_eq!(subject.snapshot().dps_report_result, Some(report()));
    assert_eq!(subject.provider_status(Provider::Twitch).attempts, 1);
}

#[test]
fn explicit_reupload_clears_only_the_selected_receipt() {
    for provider in [Provider::DpsReport, Provider::Wingman, Provider::DonBot] {
        let mut record = job().snapshot().clone();
        record.encounter_metadata = Some(metadata());
        record.dps_report_result = Some(report());
        record.wingman_upload_receipt = Some(WingmanUploadReceipt {
            permalink: "https://example.invalid/report".into(),
        });
        record.donbot_upload_receipt = Some(DonBotUploadReceipt::default());
        record.twitch_delivery_receipt = Some(sent_receipt());
        for status in &mut record.providers {
            status.state = ProviderState::Succeeded;
        }
        let mut subject = UploadJob::restore(UploadJobId::new(1).unwrap(), record.clone()).unwrap();
        subject.prepare_explicit_delivery(provider).unwrap();
        let current = subject.snapshot();
        assert_eq!(
            current.dps_report_result.is_none(),
            provider == Provider::DpsReport
        );
        assert_eq!(
            current.wingman_upload_receipt.is_none(),
            provider == Provider::Wingman
        );
        assert_eq!(
            current.donbot_upload_receipt.is_none(),
            provider == Provider::DonBot
        );
        assert_eq!(
            current.twitch_delivery_receipt,
            record.twitch_delivery_receipt
        );
        assert_eq!(
            subject.provider_status(provider).state,
            ProviderState::Waiting
        );
    }
}

#[test]
fn provider_labels_and_terminal_states_match_the_existing_core() {
    assert_eq!(
        Provider::ALL.map(Provider::name),
        ["dps.report", "GW2Wingman", "DonBot", "Twitch"]
    );
    for state in [
        ProviderState::Disabled,
        ProviderState::Succeeded,
        ProviderState::Skipped,
        ProviderState::Cancelled,
    ] {
        assert!(state.is_terminal());
    }
    for state in [
        ProviderState::Waiting,
        ProviderState::Active,
        ProviderState::RetryScheduled(Instant::now()),
        ProviderState::Failed,
    ] {
        assert!(!state.is_terminal());
    }
}
