use manny_uploader::{application::*, domain::*};
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
    time::{Instant, SystemTime},
};

type State = DonBotAggregateDeliveryState;
type Error = DonBotAggregateDeliveryError;
struct TestClock;
impl Clock for TestClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}
struct Provider(manny_uploader::domain::Provider);
impl UploadProvider for Provider {
    fn provider(&self) -> manny_uploader::domain::Provider {
        self.0
    }
    fn enqueue(&mut self, _: UploadRequest) -> Result<(), ProviderError> {
        Ok(())
    }
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        Ok(None)
    }
    fn cancel_pending(&mut self) {}
}
struct Ordinary(Settings);
impl SettingsStore for Ordinary {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        Ok(SettingsLoadResult {
            settings: self.0.clone(),
            source: SettingsLoadSource::Primary,
            recovery: None,
        })
    }
    fn save(&self, _: &Settings) -> Result<(), SettingsStoreError> {
        Ok(())
    }
}
#[derive(Default)]
struct Delivery {
    requests: Vec<DonBotAggregateRequest>,
    results: VecDeque<DonBotAggregateResult>,
    reject: bool,
    panic_enqueue: bool,
    panic_poll: bool,
    cancelled: usize,
}
struct Port(Arc<Mutex<Delivery>>);
impl DonBotAggregateDelivery for Port {
    fn enqueue(
        &mut self,
        request: DonBotAggregateRequest,
    ) -> Result<(), DonBotAggregateDispatchError> {
        let mut state = self.0.lock().unwrap();
        if state.reject {
            return Err(DonBotAggregateDispatchError::Busy);
        }
        state.requests.push(request);
        if state.panic_enqueue {
            drop(state);
            panic!("synthetic queue panic");
        }
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<DonBotAggregateResult> {
        let mut state = self.0.lock().unwrap();
        if state.panic_poll {
            drop(state);
            panic!("synthetic poll panic");
        }
        state.results.pop_front()
    }
    fn busy(&self) -> bool {
        false
    }
    fn cancel_pending(&mut self) {
        let mut state = self.0.lock().unwrap();
        state.cancelled += 1;
        state.results.clear();
    }
}
struct Harness {
    uploads: UploadCoordinator<TestClock>,
    configuration: ConfigurationService,
    donbot: DonBotConfigurationSnapshot,
    delivery: Arc<Mutex<Delivery>>,
    controller: DonBotAggregateDeliveryController,
    jobs: Vec<UploadJobId>,
}
impl Harness {
    fn new() -> Self {
        let mut settings = Settings::new("synthetic/logs".into());
        settings.donbot.enabled = true;
        settings.donbot.api_base_url = "https://donbot.invalid".into();
        settings.donbot.selected_guild_id = "123".into();
        let configuration = ConfigurationService::new(
            Box::new(Ordinary(settings)),
            Err(SecretStoreError {
                id: None,
                code: SecretStoreErrorCode::InvalidConfiguration,
                system_error: None,
            }),
        )
        .unwrap();
        let donbot = DonBotConfigurationSnapshot {
            state: DonBotConfigurationState::Verified,
            api_base_url: "https://donbot.invalid".into(),
            account_name: Some("Synthetic.1234".into()),
            discord_summary_delivery_v1: true,
            discord_aggregate_delivery_v1: true,
            guilds: vec![DonBotGuild {
                guild_id: "123".into(),
                guild_name: "Synthetic guild".into(),
                discord_delivery: DonBotDiscordDeliveryPolicy {
                    enabled: true,
                    defaults_available: true,
                    channel_override_allowed: true,
                    aggregate_enabled: true,
                    aggregate_defaults_available: true,
                    max_aggregate_fight_logs: 3,
                    channels: vec![DonBotChannel {
                        channel_id: "456".into(),
                        channel_name: "Synthetic channel".into(),
                    }],
                    ..Default::default()
                },
            }],
            selected_guild_id: "123".into(),
            diagnostic: None,
            refreshing: false,
            revision: 7,
            shutting_down: false,
        };
        let delivery = Arc::new(Mutex::new(Delivery::default()));
        let controller = DonBotAggregateDeliveryController::new(Box::new(Port(delivery.clone())));
        let mut result = Self {
            uploads: UploadCoordinator::new(
                TestClock,
                manny_uploader::domain::Provider::ALL
                    .into_iter()
                    .map(|provider| Box::new(Provider(provider)) as Box<dyn UploadProvider>)
                    .collect(),
                4,
            )
            .unwrap(),
            configuration,
            donbot,
            delivery,
            controller,
            jobs: vec![],
        };
        result.add(Some(101), None);
        result.add(Some(102), Some("999"));
        result
    }
    fn add(&mut self, fight: Option<u64>, guild: Option<&str>) -> UploadJobId {
        let id = self
            .uploads
            .add_pending(
                LogFileIdentity {
                    canonical_path: format!("synthetic/{}.zevtc", self.jobs.len()).into(),
                    size: 10,
                    last_write_time: SystemTime::UNIX_EPOCH,
                },
                ProviderSelection {
                    donbot: true,
                    ..Default::default()
                },
            )
            .unwrap();
        self.uploads
            .set_metadata(
                id,
                EncounterMetadata {
                    boss_id: 1,
                    pov_account: "Synthetic.1234".into(),
                    remaining_health_basis_points: None,
                },
            )
            .unwrap();
        self.uploads
            .apply_result(UploadResult {
                job_id: id,
                provider: manny_uploader::domain::Provider::DonBot,
                attempt: 1,
                outcome: UploadOutcome::Succeeded(UploadSuccess::DonBot(Some(
                    DonBotUploadReceipt {
                        upload_id: Some(1000),
                        fight_log_id: fight,
                        discord_delivery: DonBotDiscordDeliveryReceipt::default(),
                        guild_id: guild.map(str::to_owned),
                    },
                ))),
            })
            .unwrap();
        self.jobs.push(id);
        id
    }
    fn command(&self) -> SendDonBotAggregateCommand {
        SendDonBotAggregateCommand {
            job_ids: self.jobs.clone(),
            configuration_revision: self.configuration.snapshot().revision,
            donbot_revision: self.donbot.revision,
        }
    }
    fn submit(&mut self) {
        self.controller.submit(self.command()).unwrap();
    }
    fn tick(&mut self) -> Result<(), Error> {
        self.controller
            .tick(&self.uploads, &self.configuration, &self.donbot)
    }
    fn reply(
        &self,
        outcome: DonBotAggregateOutcome,
        receipt: Option<DonBotDiscordDeliveryReceipt>,
    ) {
        let mut state = self.delivery.lock().unwrap();
        let request = state.requests.last().unwrap().clone();
        state.results.push_back(DonBotAggregateResult {
            request_id: request.request_id,
            outcome,
            fight_log_count: request.fight_log_ids.len(),
            discord_delivery: receipt,
            credential_failure: None,
        });
    }
    fn assert_rejected(&mut self, error: Error) {
        self.tick().unwrap();
        assert_eq!(self.controller.snapshot().state, State::Failed);
        assert_eq!(self.controller.snapshot().diagnostic, Some(error));
        assert!(self.delivery.lock().unwrap().requests.is_empty());
    }
}
fn sent() -> DonBotDiscordDeliveryReceipt {
    DonBotDiscordDeliveryReceipt {
        outcome: DonBotDiscordDeliveryOutcome::Sent,
        sent: 2,
        ..Default::default()
    }
}

#[test]
fn explicit_action_resolves_current_jobs_and_ignores_historical_guild_provenance() {
    let mut h = Harness::new();
    let before = h.uploads.snapshots();
    h.submit();
    assert_eq!(h.controller.snapshot().state, State::Queued);
    assert!(h.delivery.lock().unwrap().requests.is_empty());
    h.tick().unwrap();
    let request = h.delivery.lock().unwrap().requests[0].clone();
    assert_eq!(request.request_id, 1);
    assert_eq!(request.fight_log_ids, [101, 102]);
    assert_eq!(request.guild_id, "123");
    assert_eq!(request.api_base_url, "https://donbot.invalid");
    assert_eq!(request.delivery, DonBotDeliveryRoute::GuildDefaults);
    assert_eq!(h.controller.snapshot().state, State::Sending);
    h.reply(DonBotAggregateOutcome::Succeeded, Some(sent()));
    h.tick().unwrap();
    assert_eq!(h.controller.snapshot().state, State::Succeeded);
    assert_eq!(h.controller.snapshot().discord_delivery, Some(sent()));
    assert_eq!(h.uploads.snapshots(), before);
    let mut detached = h.controller.snapshot();
    detached.state = State::Failed;
    assert_ne!(detached, h.controller.snapshot());
    assert_eq!(h.controller.snapshot().state, State::Succeeded);
}

#[test]
fn only_one_command_is_admitted_and_stale_results_do_not_finish_current_request() {
    let mut h = Harness::new();
    h.submit();
    assert_eq!(h.controller.submit(h.command()), Err(Error::Busy));
    h.tick().unwrap();
    assert_eq!(h.controller.submit(h.command()), Err(Error::Busy));
    let before = h.controller.snapshot();
    h.delivery
        .lock()
        .unwrap()
        .results
        .push_back(DonBotAggregateResult {
            request_id: 99,
            outcome: DonBotAggregateOutcome::Failed,
            fight_log_count: 2,
            discord_delivery: None,
            credential_failure: None,
        });
    h.tick().unwrap();
    assert_eq!(h.controller.snapshot(), before);
    h.reply(DonBotAggregateOutcome::Succeeded, Some(sent()));
    h.tick().unwrap();
    h.submit();
    h.tick().unwrap();
    assert_eq!(h.delivery.lock().unwrap().requests[1].request_id, 2);
}

#[test]
fn malformed_commands_are_rejected_before_queueing() {
    let mut h = Harness::new();
    for mutation in 0..5 {
        let mut command = h.command();
        match mutation {
            0 => command.configuration_revision = 0,
            1 => command.donbot_revision = 0,
            2 => command.job_ids.pop().map(|_| ()).unwrap(),
            3 => command.job_ids[1] = command.job_ids[0],
            4 => command.job_ids = (1..=101).map(|id| UploadJobId::new(id).unwrap()).collect(),
            _ => unreachable!(),
        }
        assert_eq!(h.controller.submit(command), Err(Error::InvalidCommand));
        assert_eq!(h.controller.snapshot().state, State::Idle);
    }
}

#[test]
fn changed_settings_or_verification_revision_rejects_queued_selection() {
    for change_settings in [true, false] {
        let mut h = Harness::new();
        h.submit();
        if change_settings {
            let mut changed = h.configuration.snapshot().settings;
            changed.dps_report.detailed_wvw = !changed.dps_report.detailed_wvw;
            h.configuration.save_settings(changed).unwrap();
        } else {
            h.donbot.revision += 1;
        }
        h.assert_rejected(Error::StaleConfiguration);
    }
}

#[test]
fn live_capability_policy_route_and_bounds_gate_dispatch() {
    for mutation in 0..13 {
        let mut h = Harness::new();
        match mutation {
            0 => h.donbot.discord_aggregate_delivery_v1 = false,
            1 => h.donbot.discord_summary_delivery_v1 = false,
            2 => h.donbot.guilds[0].discord_delivery.aggregate_enabled = false,
            3 => {
                h.donbot.guilds[0]
                    .discord_delivery
                    .aggregate_defaults_available = false
            }
            4 => h.donbot.guilds[0].discord_delivery.defaults_available = false,
            5 => h.donbot.guilds[0].discord_delivery.max_aggregate_fight_logs = 1,
            6 => h.donbot.guilds[0].discord_delivery.max_aggregate_fight_logs = 101,
            7 => h.donbot.state = DonBotConfigurationState::Unverified,
            8 => h.donbot.refreshing = true,
            9 => h.donbot.guilds.clear(),
            10 => h.donbot.api_base_url = "https://other.invalid".into(),
            11 => h.donbot.selected_guild_id = "999".into(),
            12 => {
                let mut settings = h.configuration.snapshot().settings;
                settings.donbot.enabled = false;
                h.configuration.save_settings(settings).unwrap();
            }
            _ => unreachable!(),
        }
        h.submit();
        h.assert_rejected(Error::Unavailable);
    }
    let mut h = Harness::new();
    h.add(Some(103), None);
    h.donbot.guilds[0].discord_delivery.max_aggregate_fight_logs = 2;
    h.submit();
    h.assert_rejected(Error::Unavailable);
}

#[test]
fn authorized_explicit_channel_does_not_require_automatic_delivery_or_message_kinds() {
    let mut h = Harness::new();
    let mut settings = h.configuration.snapshot().settings;
    settings.donbot.discord_channel_override_explicit = true;
    settings.donbot.selected_discord_channel_id = "456".into();
    h.configuration.save_settings(settings).unwrap();
    h.donbot.guilds[0].discord_delivery.defaults_available = false;
    h.donbot.guilds[0]
        .discord_delivery
        .aggregate_defaults_available = false;
    h.submit();
    h.tick().unwrap();
    assert_eq!(
        h.delivery.lock().unwrap().requests[0].delivery,
        DonBotDeliveryRoute::ChannelOverride {
            channel_id: "456".into()
        }
    );
}

#[test]
fn evicted_jobs_missing_fights_and_duplicate_fights_are_rechecked_at_dispatch() {
    let mut h = Harness::new();
    h.submit();
    h.uploads.set_history_limit(1).unwrap();
    h.assert_rejected(Error::UnknownJob);
    for fight in [None, Some(101)] {
        let mut h = Harness::new();
        h.add(fight, None);
        h.submit();
        h.assert_rejected(Error::IneligibleJob);
    }
    let mut h = Harness::new();
    h.submit();
    h.uploads.reupload(h.jobs[0]).unwrap();
    h.assert_rejected(Error::IneligibleJob);
}

#[test]
fn every_settled_result_is_session_only_and_never_retries_without_another_command() {
    let cases = [
        (DonBotAggregateOutcome::Failed, State::Failed, None),
        (DonBotAggregateOutcome::Ambiguous, State::Ambiguous, None),
        (DonBotAggregateOutcome::Cancelled, State::Cancelled, None),
        (
            DonBotAggregateOutcome::Succeeded,
            State::Succeeded,
            Some(sent()),
        ),
        (
            DonBotAggregateOutcome::Succeeded,
            State::Succeeded,
            Some(DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: 1,
                failed: 1,
                ..Default::default()
            }),
        ),
        (
            DonBotAggregateOutcome::Failed,
            State::Failed,
            Some(DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Skipped,
                skipped: 2,
                ..Default::default()
            }),
        ),
        (
            DonBotAggregateOutcome::Ambiguous,
            State::Ambiguous,
            Some(DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: 1,
                ambiguous: 1,
                ..Default::default()
            }),
        ),
    ];
    for (outcome, state, receipt) in cases {
        let mut h = Harness::new();
        let before = h.uploads.history();
        h.submit();
        h.tick().unwrap();
        h.reply(outcome, receipt.clone());
        h.tick().unwrap();
        assert_eq!(h.controller.snapshot().state, state);
        assert_eq!(h.controller.snapshot().discord_delivery, receipt);
        for _ in 0..3 {
            h.tick().unwrap();
        }
        assert_eq!(h.delivery.lock().unwrap().requests.len(), 1);
        assert_eq!(h.uploads.history(), before);
        h.submit();
        h.tick().unwrap();
        assert_eq!(h.delivery.lock().unwrap().requests.len(), 2);
    }
}

#[test]
fn invalid_result_counts_receipts_and_outcomes_settle_as_ambiguous() {
    for mutation in 0..5 {
        let mut h = Harness::new();
        h.submit();
        h.tick().unwrap();
        h.reply(DonBotAggregateOutcome::Succeeded, Some(sent()));
        let mut delivery = h.delivery.lock().unwrap();
        let result = delivery.results.front_mut().unwrap();
        match mutation {
            0 => result.fight_log_count = 3,
            1 => result.discord_delivery = None,
            2 => result.discord_delivery = Some(DonBotDiscordDeliveryReceipt::default()),
            3 => result.discord_delivery.as_mut().unwrap().sent = 101,
            4 => result.outcome = DonBotAggregateOutcome::Failed,
            _ => unreachable!(),
        }
        drop(delivery);
        h.tick().unwrap();
        assert_eq!(h.controller.snapshot().state, State::Ambiguous);
        assert!(h.controller.snapshot().discord_delivery.is_none());
        h.tick().unwrap();
        assert_eq!(h.delivery.lock().unwrap().requests.len(), 1);
    }
}

#[test]
fn credential_failures_retain_safe_notice_and_rejection_does_not_retry() {
    let mut h = Harness::new();
    h.submit();
    h.tick().unwrap();
    h.reply(DonBotAggregateOutcome::Failed, None);
    h.delivery.lock().unwrap().results[0].credential_failure =
        Some(UploadFailure::DonBotKeyMissing);
    h.tick().unwrap();
    assert_eq!(
        h.controller.snapshot().credential_failure,
        Some(UploadFailure::DonBotKeyMissing)
    );
    assert_eq!(
        h.controller.snapshot().detail(),
        UploadFailure::DonBotKeyMissing.detail()
    );
    h.delivery.lock().unwrap().reject = true;
    h.submit();
    h.tick().unwrap();
    assert_eq!(
        h.controller.snapshot().diagnostic,
        Some(Error::DispatchFailed)
    );
    assert_eq!(h.controller.snapshot().credential_failure, None);
    h.tick().unwrap();
    assert_eq!(h.delivery.lock().unwrap().requests.len(), 1);
}

#[test]
fn panics_after_acceptance_or_while_awaiting_result_are_ambiguous() {
    for enqueue in [true, false] {
        let mut h = Harness::new();
        h.submit();
        h.delivery.lock().unwrap().panic_enqueue = enqueue;
        h.tick().unwrap();
        if !enqueue {
            h.delivery.lock().unwrap().panic_poll = true;
            h.tick().unwrap();
        }
        assert_eq!(h.controller.snapshot().state, State::Ambiguous);
        assert_eq!(h.delivery.lock().unwrap().requests.len(), 1);
        assert!(!h.controller.snapshot().detail().contains("synthetic"));
    }
    let mut h = Harness::new();
    h.submit();
    h.delivery.lock().unwrap().panic_poll = true;
    h.tick().unwrap();
    assert_eq!(h.controller.snapshot().state, State::Failed);
    assert!(h.delivery.lock().unwrap().requests.is_empty());
}

#[test]
fn shutdown_discards_work_cancels_once_and_rejects_later_commands() {
    for active in [true, false] {
        let mut h = Harness::new();
        h.submit();
        if active {
            h.tick().unwrap();
        }
        h.controller.shutdown();
        let before = h.controller.snapshot();
        assert_eq!(before.state, State::ShuttingDown);
        assert!(before.shutting_down);
        h.controller.shutdown();
        assert_eq!(h.controller.snapshot(), before);
        assert_eq!(h.controller.submit(h.command()), Err(Error::ShuttingDown));
        assert_eq!(h.tick(), Err(Error::ShuttingDown));
        let delivery = h.delivery.clone();
        drop(h);
        assert_eq!(delivery.lock().unwrap().cancelled, 1);
    }
    let mut h = Harness::new();
    h.submit();
    h.configuration.shutdown();
    assert_eq!(h.tick(), Err(Error::ShuttingDown));
    assert!(h.delivery.lock().unwrap().requests.is_empty());
}
