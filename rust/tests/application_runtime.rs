use manny_uploader::{application::*, domain::*, providers::TwitchClient};
use std::{
    collections::VecDeque,
    path::Path,
    sync::{Arc, Mutex},
    time::{Duration, Instant, SystemTime},
};
type Command = NexusOptionsCommand;

#[derive(Clone)]
struct TestClock(Arc<Mutex<(Instant, SystemTime)>>);
impl TestClock {
    fn new() -> Self {
        Self(Arc::new(Mutex::new((
            Instant::now(),
            SystemTime::UNIX_EPOCH + Duration::from_secs(864000),
        ))))
    }
    fn advance(&self, seconds: u64) {
        let mut time = self.0.lock().unwrap();
        time.0 += Duration::from_secs(seconds);
        time.1 += Duration::from_secs(seconds);
    }
}
impl Clock for TestClock {
    fn steady_now(&self) -> Instant {
        self.0.lock().unwrap().0
    }
    fn system_now(&self) -> SystemTime {
        self.0.lock().unwrap().1
    }
}
#[derive(Default)]
struct Events {
    source: Option<CandidateSourceOptions>,
    polls: usize,
    observations: Vec<LogFileIdentity>,
    fail_source: bool,
    poll_entered: Option<std::sync::mpsc::Sender<()>>,
    parser_capacity: usize,
    parsing: VecDeque<MetadataRequest>,
    parsed: VecDeque<MetadataResult>,
    configs: Vec<ProviderConfiguration>,
    uploads: [VecDeque<UploadRequest>; 4],
    uploaded: [VecDeque<UploadResult>; 4],
    fail_provider: Option<Provider>,
    history: Vec<UploadJobRecord>,
    history_writes: usize,
    fail_history: bool,
    settings_writes: usize,
    client_id: String,
    client_changes: Vec<String>,
    fail_identity: bool,
    panic_identity: bool,
    immediate_verification: Option<DonBotVerification>,
    verification_calls: usize,
    auth: VecDeque<TwitchAuthenticationRequest>,
    authenticated: VecDeque<TwitchAuthenticationResult>,
    auth_identities: Vec<String>,
    verification: VecDeque<DonBotVerificationRequest>,
    verified: VecDeque<DonBotVerificationResult>,
    aggregate: Vec<DonBotAggregateRequest>,
    launched: Vec<String>,
    cancellations: usize,
    secret_operations: Vec<&'static str>,
}
struct Store(Arc<Mutex<Events>>, Settings);
impl SettingsStore for Store {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        Ok(SettingsLoadResult {
            settings: self.1.clone(),
            source: SettingsLoadSource::Primary,
            recovery: None,
        })
    }
    fn save(&self, _: &Settings) -> Result<(), SettingsStoreError> {
        self.0.lock().unwrap().settings_writes += 1;
        Ok(())
    }
}
struct Secrets(Arc<Mutex<Events>>);
impl SecretStore for Secrets {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::NativeUserScoped
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        self.0.lock().unwrap().secret_operations.push("load");
        if id == SecretId::DonBotGw2ApiKey {
            return Ok(SecretValue::from_bytes(b"SYNTHETIC-KEY".to_vec()));
        }
        Err(SecretStoreError {
            id: Some(id),
            code: SecretStoreErrorCode::NotFound,
            system_error: None,
        })
    }
    fn store(&mut self, _: SecretId, _: &SecretValue) -> Result<(), SecretStoreError> {
        self.0.lock().unwrap().secret_operations.push("store");
        Ok(())
    }
    fn erase(&mut self, _: SecretId) -> Result<(), SecretStoreError> {
        self.0.lock().unwrap().secret_operations.push("erase");
        Ok(())
    }
}
struct Source(Arc<Mutex<Events>>);
impl LogCandidateSource for Source {
    fn reconfigure(&mut self, options: CandidateSourceOptions) -> Result<(), CandidateError> {
        let mut events = self.0.lock().unwrap();
        if events.fail_source {
            return Err(CandidateError::ScanFailed);
        }
        events.source = Some(options);
        Ok(())
    }
    fn poll(&mut self, stop: &StopToken) -> Result<CandidateBatch, CandidateError> {
        let mut events = self.0.lock().unwrap();
        events.polls += 1;
        if let Some(entered) = events.poll_entered.take() {
            drop(events);
            entered.send(()).unwrap();
            if stop.wait_for_stop(Duration::from_secs(5)) {
                return Err(CandidateError::Cancelled);
            }
            events = self.0.lock().unwrap();
        }
        if events.fail_source {
            return Err(CandidateError::ScanFailed);
        }
        let cutoff = events
            .source
            .as_ref()
            .and_then(|source| source.minimum_last_write_time);
        Ok(CandidateBatch {
            root_available: true,
            scan_complete: true,
            observations: events
                .observations
                .iter()
                .filter(|file| cutoff.is_none_or(|cutoff| file.last_write_time >= cutoff))
                .cloned()
                .collect(),
            ..Default::default()
        })
    }
}
struct Parser(Arc<Mutex<Events>>);
impl LogMetadataParser for Parser {
    fn update_queue_capacity(&mut self, capacity: usize) -> Result<(), ParserError> {
        self.0.lock().unwrap().parser_capacity = capacity;
        Ok(())
    }
    fn enqueue(&mut self, request: MetadataRequest) -> Result<(), ParserError> {
        self.0.lock().unwrap().parsing.push_back(request);
        Ok(())
    }
    fn available_capacity(&self) -> usize {
        let events = self.0.lock().unwrap();
        events.parser_capacity.saturating_sub(events.parsing.len())
    }
    fn try_take_result(&mut self) -> Result<Option<MetadataResult>, ParserError> {
        Ok(self.0.lock().unwrap().parsed.pop_front())
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().cancellations += 1;
    }
}
struct ProviderPort(Provider, Arc<Mutex<Events>>);
impl UploadProvider for ProviderPort {
    fn provider(&self) -> Provider {
        self.0
    }
    fn configure(
        &mut self,
        configuration: ProviderConfiguration,
    ) -> Result<(), ProviderConfigurationError> {
        let mut events = self.1.lock().unwrap();
        if events.fail_provider == Some(self.0) {
            return Err(ProviderConfigurationError::ResourceLimit);
        }
        events.configs.push(configuration);
        Ok(())
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        self.1.lock().unwrap().uploads[self.0 as usize].push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        Ok(self.1.lock().unwrap().uploaded[self.0 as usize].pop_front())
    }
    fn cancel_pending(&mut self) {
        self.1.lock().unwrap().cancellations += 1;
    }
}
struct History(Arc<Mutex<Events>>, Vec<UploadJobRecord>);
impl UploadHistoryStore for History {
    fn records(&self) -> &[UploadJobRecord] {
        &self.1
    }
    fn recovery(&self) -> Option<HistoryRecovery> {
        None
    }
    fn merge_and_save(&mut self, updates: &[UploadJobRecord]) -> Result<(), HistoryStoreError> {
        let mut events = self.0.lock().unwrap();
        if events.fail_history {
            return Err(HistoryStoreError::SerializationFailed);
        }
        for update in updates {
            if let Some(record) = self.1.iter_mut().find(|record| record.file == update.file) {
                *record = update.clone();
            } else {
                self.1.push(update.clone());
            }
        }
        events.history = self.1.clone();
        events.history_writes += 1;
        Ok(())
    }
}
struct Identity(Arc<Mutex<Events>>);
impl TwitchApplicationConfiguration for Identity {
    fn set_client_id(&self, id: String) -> Result<(), TwitchError> {
        let mut events = self.0.lock().unwrap();
        if events.panic_identity {
            drop(events);
            panic!("synthetic identity failure");
        }
        if events.fail_identity {
            return Err(TwitchError::failed(
                TwitchOperation::Start,
                TwitchErrorCode::Internal,
            ));
        }
        events.client_id = id.clone();
        events.client_changes.push(id);
        Ok(())
    }
}
struct Auth(Arc<Mutex<Events>>);
impl TwitchAuthenticator for Auth {
    fn enqueue(
        &mut self,
        request: TwitchAuthenticationRequest,
    ) -> Result<(), TwitchAuthenticationDispatchError> {
        let mut events = self.0.lock().unwrap();
        let id = events.client_id.clone();
        events.auth_identities.push(id);
        events.auth.push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<TwitchAuthenticationResult> {
        self.0.lock().unwrap().authenticated.pop_front()
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().cancellations += 1;
    }
}
struct Verifier(Arc<Mutex<Events>>);
impl DonBotVerifier for Verifier {
    fn enqueue(
        &mut self,
        request: DonBotVerificationRequest,
    ) -> Result<(), DonBotVerificationDispatchError> {
        let mut events = self.0.lock().unwrap();
        events.verification_calls += 1;
        if let Some(identity) = events.immediate_verification.clone() {
            events.verified.push_back(DonBotVerificationResult {
                request_id: request.request_id,
                verification: Ok(DonBotVerificationSuccess {
                    api_key: request.api_key,
                    identity,
                }),
            });
        } else {
            events.verification.push_back(request);
        }
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<DonBotVerificationResult> {
        self.0.lock().unwrap().verified.pop_front()
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().cancellations += 1;
    }
}
struct TestMessenger(Arc<Mutex<Events>>);
impl TwitchTestMessenger for TestMessenger {
    fn enqueue(
        &mut self,
        _: TwitchTestMessageRequest,
    ) -> Result<(), TwitchTestMessageDispatchError> {
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<TwitchTestMessageResult> {
        None
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().cancellations += 1;
    }
}
struct Aggregate(Arc<Mutex<Events>>);
impl DonBotAggregateDelivery for Aggregate {
    fn busy(&self) -> bool {
        !self.0.lock().unwrap().aggregate.is_empty()
    }
    fn enqueue(
        &mut self,
        request: DonBotAggregateRequest,
    ) -> Result<(), DonBotAggregateDispatchError> {
        self.0.lock().unwrap().aggregate.push(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<DonBotAggregateResult> {
        None
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().cancellations += 1;
    }
}
struct Launcher(Arc<Mutex<Events>>);
impl ExternalActionLauncher for Launcher {
    fn open_url(&mut self, url: &str) -> Result<(), ExternalActionError> {
        self.0.lock().unwrap().launched.push(url.into());
        Ok(())
    }
    fn open_directory(&mut self, _: &Path) -> Result<(), ExternalActionError> {
        Ok(())
    }
}
struct NoHttp;
impl HttpClient for NoHttp {
    fn execute(&self, _: HttpRequest, _: &StopToken) -> Result<HttpResponse, HttpError> {
        panic!("runtime must use asynchronous ports")
    }
}
struct Harness {
    runtime: ApplicationRuntime<TestClock>,
    handle: RuntimeHandle,
    events: Arc<Mutex<Events>>,
    clock: TestClock,
    stop: CancellationSource,
}
impl Harness {
    fn new(settings: Settings, history: Vec<UploadJobRecord>) -> Self {
        Self::build(settings, history, |_| {}).unwrap()
    }
    fn build(
        settings: Settings,
        history: Vec<UploadJobRecord>,
        setup: impl FnOnce(&mut Events),
    ) -> Result<Self, RuntimeError> {
        let events = Arc::new(Mutex::new(Events {
            history: history.clone(),
            ..Default::default()
        }));
        setup(&mut events.lock().unwrap());
        let config = Arc::new(Mutex::new(
            ConfigurationService::new(
                Box::new(Store(events.clone(), settings.clone())),
                Ok(Box::new(Secrets(events.clone()))),
            )
            .unwrap(),
        ));
        let clock = TestClock::new();
        let shared_clock = Arc::new(clock.clone());
        let api = Arc::new(TwitchClient::new(Arc::new(NoHttp), "unused".into()).unwrap());
        let owner = Arc::new(TwitchSessionOwner::new(
            config.clone(),
            api,
            shared_clock.clone(),
        ));
        let twitch = TwitchAuthenticationController::new(
            config.clone(),
            Box::new(Auth(events.clone())),
            owner,
            shared_clock.clone(),
        )
        .unwrap();
        let donbot = DonBotConfigurationController::new(
            &config.lock().unwrap(),
            Box::new(Verifier(events.clone())),
        )
        .unwrap();
        let options = NexusOptionsController::new(
            config.clone(),
            donbot,
            twitch,
            TwitchTestMessageController::new(Box::new(TestMessenger(events.clone()))),
            Box::new(Launcher(events.clone())),
            Default::default(),
        )
        .unwrap();
        let uploads = UploadCoordinator::new(
            clock.clone(),
            Provider::ALL
                .into_iter()
                .map(|provider| {
                    Box::new(ProviderPort(provider, events.clone())) as Box<dyn UploadProvider>
                })
                .collect(),
            settings.general.recent_log_limit as usize,
        )
        .unwrap();
        let ingestion = LogIngestionCoordinator::new(uploads, Box::new(Parser(events.clone())));
        let pump = ApplicationPump::new(
            Box::new(Source(events.clone())),
            ingestion,
            Default::default(),
        )
        .unwrap();
        let runtime = ApplicationRuntime::new(
            RuntimeParts {
                pump,
                options,
                aggregate: DonBotAggregateDeliveryController::new(Box::new(Aggregate(
                    events.clone(),
                ))),
                history: Box::new(History(events.clone(), history)),
                configuration: config,
                twitch_application: Arc::new(Identity(events.clone())),
                launcher: Box::new(Launcher(events.clone())),
                clock: shared_clock,
            },
            "bundledpublic".into(),
        )?;
        let handle = runtime.handle();
        Ok(Self {
            runtime,
            handle,
            events,
            clock,
            stop: CancellationSource::default(),
        })
    }
    fn standard() -> Self {
        Self::new(Settings::new("synthetic-logs".into()), vec![])
    }
    fn tick(&mut self) {
        self.runtime.tick(&self.stop.token()).unwrap();
    }
    fn run(&mut self, command: Command) {
        self.handle.submit_options(command).unwrap();
        self.tick();
    }
    fn file(&self, id: u64) -> LogFileIdentity {
        LogFileIdentity {
            canonical_path: format!("synthetic-logs/{id}.zevtc").into(),
            size: id,
            last_write_time: self.clock.system_now(),
        }
    }
    fn admit(&mut self) {
        self.events.lock().unwrap().observations.push(self.file(1));
        self.tick();
        self.clock.advance(1);
        self.tick();
    }
    fn parse(&self) {
        let mut events = self.events.lock().unwrap();
        let request = events.parsing.pop_front().unwrap();
        events.parsed.push_back(MetadataResult {
            job_id: request.job_id,
            metadata: Ok(EncounterMetadata {
                boss_id: 42,
                pov_account: "Player.1234".into(),
                remaining_health_basis_points: None,
            }),
        });
    }
    fn complete(&self, provider: Provider, outcome: UploadOutcome) {
        let mut events = self.events.lock().unwrap();
        let request = events.uploads[provider as usize].pop_front().unwrap();
        events.uploaded[provider as usize].push_back(UploadResult {
            job_id: request.job_id,
            provider,
            attempt: request.attempt,
            outcome,
        });
    }
    fn verify(&self) {
        let mut events = self.events.lock().unwrap();
        let request = events.verification.pop_front().unwrap();
        events.verified.push_back(DonBotVerificationResult {
            request_id: request.request_id,
            verification: Ok(DonBotVerificationSuccess {
                api_key: request.api_key,
                identity: DonBotVerification {
                    account_name: "Player.1234".into(),
                    discord_summary_delivery_v1: true,
                    discord_aggregate_delivery_v1: true,
                    guilds: vec![DonBotGuild {
                        guild_id: "123".into(),
                        guild_name: "Guild".into(),
                        discord_delivery: DonBotDiscordDeliveryPolicy {
                            enabled: true,
                            defaults_available: true,
                            pve_summary: true,
                            aggregate_enabled: true,
                            aggregate_defaults_available: true,
                            max_aggregate_fight_logs: 100,
                            ..Default::default()
                        },
                    }],
                },
            }),
        });
    }
}

#[test]
fn runtime_construction_contains_adapter_panics() {
    let result = Harness::build(Settings::new("synthetic-logs".into()), vec![], |events| {
        events.panic_identity = true
    });
    assert!(matches!(result, Err(RuntimeError::Internal)));
}

#[test]
fn application_thread_shutdown_interrupts_owned_work_and_joins_before_returning() {
    let h = Harness::standard();
    let (entered, waiting) = std::sync::mpsc::channel();
    h.events.lock().unwrap().poll_entered = Some(entered);
    let mut owner = ApplicationRuntimeThread::start(h.runtime).unwrap();
    waiting.recv_timeout(Duration::from_secs(5)).unwrap();
    let handle = owner.handle();
    handle
        .submit_options(Command::SetWindowVisible(false))
        .unwrap();
    owner.shutdown();
    let snapshot = handle.snapshot();
    assert!(snapshot.shutting_down);
    assert!(
        snapshot
            .options
            .configuration
            .settings
            .general
            .window_visible
    );
    let cancellations = h.events.lock().unwrap().cancellations;
    assert_eq!(cancellations, 9);
    drop(owner);
    assert_eq!(h.events.lock().unwrap().cancellations, cancellations);
    assert_eq!(
        handle.select_logs(LogSelectionMode::Last24Hours),
        Err(RuntimeError::ShuttingDown)
    );
}

#[test]
fn same_tick_refresh_completion_resets_the_five_minute_deadline() {
    let mut settings = Settings::new("synthetic-logs".into());
    settings.donbot.enabled = true;
    settings.donbot.selected_guild_id = "123".into();
    let mut h = Harness::new(settings, vec![]);
    h.verify();
    h.tick();
    let bot = h.handle.snapshot().options.donbot;
    h.events.lock().unwrap().immediate_verification = Some(DonBotVerification {
        account_name: bot.account_name.unwrap(),
        guilds: bot.guilds,
        discord_summary_delivery_v1: true,
        discord_aggregate_delivery_v1: true,
    });
    h.clock.advance(299);
    h.run(Command::RefreshDonBot);
    let calls = h.events.lock().unwrap().verification_calls;
    h.clock.advance(1);
    h.tick();
    assert_eq!(h.events.lock().unwrap().verification_calls, calls);
    h.clock.advance(299);
    h.tick();
    assert_eq!(h.events.lock().unwrap().verification_calls, calls + 1);
}
fn report() -> DpsReportResult {
    DpsReportResult {
        permalink: "https://dps.report/fixture".into(),
        encounter_name: "Boss".into(),
        boss_id: 42,
        mode: String::new(),
        success: true,
    }
}
fn record(id: u64) -> UploadJobRecord {
    UploadJobRecord {
        file: LogFileIdentity {
            canonical_path: format!("synthetic-logs/{id}.zevtc").into(),
            size: id,
            last_write_time: SystemTime::UNIX_EPOCH + Duration::from_secs(864000),
        },
        detected_at: SystemTime::UNIX_EPOCH,
        encounter_metadata: Some(EncounterMetadata {
            boss_id: 42,
            pov_account: "Player.1234".into(),
            remaining_health_basis_points: None,
        }),
        dps_report_result: Some(report()),
        wingman_upload_receipt: None,
        donbot_upload_receipt: Some(DonBotUploadReceipt {
            fight_log_id: Some(100 + id),
            ..Default::default()
        }),
        twitch_delivery_receipt: None,
        providers: std::array::from_fn(|index| ProviderStatus {
            state: if index == 0 || index == 2 {
                ProviderState::Succeeded
            } else {
                ProviderState::Disabled
            },
            attempts: 1,
            detail: String::new(),
        }),
    }
}

#[test]
fn accepted_identity_is_persisted_before_metadata_dispatch_and_results_drain_without_scanning() {
    let mut h = Harness::standard();
    h.admit();
    assert_eq!(h.events.lock().unwrap().history.len(), 1);
    assert!(
        h.events.lock().unwrap().history[0]
            .encounter_metadata
            .is_none()
    );
    assert!(
        h.events
            .lock()
            .unwrap()
            .uploads
            .iter()
            .all(VecDeque::is_empty)
    );
    let polls = h.events.lock().unwrap().polls;
    h.parse();
    h.tick();
    assert_eq!(h.events.lock().unwrap().polls, polls);
    assert_eq!(h.events.lock().unwrap().uploads[0].len(), 1);
    h.complete(
        Provider::DpsReport,
        UploadOutcome::Succeeded(UploadSuccess::DpsReport(report())),
    );
    h.tick();
    assert_eq!(h.events.lock().unwrap().uploads[1].len(), 1);
    assert_eq!(
        h.events.lock().unwrap().history[0].dps_report_result,
        Some(report())
    );
    assert_eq!(h.events.lock().unwrap().polls, polls);
}

#[test]
fn failed_history_write_blocks_dispatch_until_the_same_identity_is_durable() {
    let mut h = Harness::standard();
    h.events.lock().unwrap().observations.push(h.file(1));
    h.tick();
    h.clock.advance(1);
    h.events.lock().unwrap().fail_history = true;
    assert!(matches!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::History(_))
    ));
    h.parse();
    for _ in 0..2 {
        assert!(matches!(
            h.runtime.tick(&h.stop.token()),
            Err(RuntimeError::History(_))
        ));
    }
    assert!(
        h.events
            .lock()
            .unwrap()
            .uploads
            .iter()
            .all(VecDeque::is_empty)
    );
    h.events.lock().unwrap().fail_history = false;
    h.tick();
    assert_eq!(h.events.lock().unwrap().uploads[0].len(), 1);
    assert_eq!(h.events.lock().unwrap().history.len(), 1);
}

#[test]
fn restart_seeds_hidden_history_and_destination_toggles_do_not_replay() {
    let mut settings = Settings::new("synthetic-logs".into());
    settings.general.recent_log_limit = 1;
    let records = vec![record(1), record(2)];
    let mut h = Harness::new(settings, records.clone());
    assert_eq!(h.handle.snapshot().jobs.len(), 1);
    h.events.lock().unwrap().observations =
        records.iter().map(|record| record.file.clone()).collect();
    h.tick();
    h.clock.advance(1);
    h.tick();
    h.run(Command::SetDpsReportEnabled(false));
    h.run(Command::SetDpsReportEnabled(true));
    h.handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    h.tick();
    h.clock.advance(1);
    h.tick();
    assert!(h.events.lock().unwrap().parsing.is_empty());
    assert!(
        h.events
            .lock()
            .unwrap()
            .uploads
            .iter()
            .all(VecDeque::is_empty)
    );
    assert_eq!(h.events.lock().unwrap().history.len(), 2);
}

#[test]
fn saved_client_id_is_applied_before_later_connect_in_the_same_tick() {
    let mut h = Harness::standard();
    assert_eq!(h.events.lock().unwrap().client_changes, ["bundledpublic"]);
    let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().options.configuration.settings);
    draft.twitch_client_id = "replacement".into();
    h.handle
        .submit_options(Command::SaveOrdinary(draft))
        .unwrap();
    h.handle.submit_options(Command::ConnectTwitch).unwrap();
    assert!(h.events.lock().unwrap().auth.is_empty());
    h.tick();
    assert_eq!(h.events.lock().unwrap().auth_identities, ["replacement"]);
}

#[test]
fn failed_identity_application_retains_later_connect_for_retry() {
    let mut h = Harness::standard();
    let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().options.configuration.settings);
    draft.twitch_client_id = "replacement".into();
    h.handle
        .submit_options(Command::SaveOrdinary(draft))
        .unwrap();
    h.handle.submit_options(Command::ConnectTwitch).unwrap();
    h.events.lock().unwrap().fail_identity = true;
    assert_eq!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::Options(
            NexusOptionsError::SettingsApplyFailed
        ))
    );
    assert!(h.events.lock().unwrap().auth.is_empty());
    assert_eq!(h.handle.snapshot().options.pending_commands, 1);
    h.events.lock().unwrap().fail_identity = false;
    h.tick();
    assert_eq!(h.events.lock().unwrap().auth_identities, ["replacement"]);
}

#[test]
fn live_settings_reconfigure_owned_components_and_retry_only_unapplied_changes() {
    let mut h = Harness::standard();
    h.tick();
    let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().options.configuration.settings);
    draft.general.log_directory = "new-synthetic-root".into();
    draft.general.parser_queue_capacity = 16;
    draft.general.parallel_uploads_per_provider = 2;
    draft.general.stability_observations = 3;
    draft.general.poll_interval_ms = 2500;
    h.events.lock().unwrap().fail_provider = Some(Provider::DonBot);
    h.handle
        .submit_options(Command::SaveOrdinary(draft))
        .unwrap();
    assert!(matches!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::Provider(_))
    ));
    let count = h.events.lock().unwrap().configs.len();
    h.events.lock().unwrap().fail_provider = None;
    h.tick();
    let events = h.events.lock().unwrap();
    assert_eq!(events.configs.len(), count + 2);
    assert_eq!(events.parser_capacity, 16);
    assert_eq!(
        events.source.as_ref().unwrap().root,
        Path::new("new-synthetic-root")
    );
    assert_eq!(events.configs.last().unwrap().parallelism, 2);
}

#[test]
fn failed_selection_reconfiguration_preserves_applied_mode_and_retries() {
    let mut h = Harness::standard();
    let cutoff = h.handle.snapshot().selection.discovery_cutoff();
    h.events.lock().unwrap().fail_source = true;
    h.handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    assert!(matches!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::Pump(_))
    ));
    assert_eq!(h.handle.snapshot().selection.mode(), LogSelectionMode::New);
    assert_eq!(h.handle.snapshot().selection.discovery_cutoff(), cutoff);
    h.events.lock().unwrap().fail_source = false;
    h.tick();
    assert_eq!(
        h.handle.snapshot().selection.mode(),
        LogSelectionMode::Last24Hours
    );
}

#[test]
fn scan_failure_remains_visible_until_a_successful_scan_resolves_it() {
    let mut h = Harness::standard();
    h.events.lock().unwrap().fail_source = true;
    assert!(matches!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::Pump(_))
    ));
    h.events.lock().unwrap().fail_source = false;
    assert!(matches!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::Pump(_))
    ));
    assert!(matches!(
        h.handle.snapshot().diagnostic,
        Some(RuntimeError::Pump(_))
    ));
    h.clock.advance(1);
    h.tick();
    assert_eq!(h.handle.snapshot().diagnostic, None);
}

#[test]
fn saved_donbot_verification_and_periodic_refresh_use_owner_time() {
    let mut settings = Settings::new("synthetic-logs".into());
    settings.donbot.enabled = true;
    settings.donbot.selected_guild_id = "123".into();
    let mut h = Harness::new(settings, vec![]);
    assert_eq!(h.events.lock().unwrap().verification.len(), 1);
    h.verify();
    h.tick();
    h.clock.advance(299);
    h.tick();
    assert!(h.events.lock().unwrap().verification.is_empty());
    h.clock.advance(1);
    h.tick();
    assert_eq!(h.events.lock().unwrap().verification.len(), 1);
    assert!(h.handle.snapshot().options.donbot.refreshing);
    h.clock.advance(10);
    h.verify();
    h.tick();
    h.clock.advance(299);
    h.tick();
    assert!(h.events.lock().unwrap().verification.is_empty());
    h.clock.advance(1);
    h.tick();
    assert_eq!(h.events.lock().unwrap().verification.len(), 1);
}

#[test]
fn row_actions_and_aggregate_submission_are_off_render_and_resolve_current_snapshots() {
    let mut settings = Settings::new("synthetic-logs".into());
    settings.donbot.enabled = true;
    settings.donbot.selected_guild_id = "123".into();
    let mut h = Harness::new(settings, vec![record(1), record(2)]);
    h.verify();
    h.tick();
    let before = h.handle.snapshot();
    h.handle
        .submit_action(RecentLogAction::OpenReport {
            job_id: before.jobs[0].id,
            target: ReportTarget::DpsReport,
        })
        .unwrap();
    let command = SendDonBotAggregateCommand {
        job_ids: before.jobs.iter().map(|job| job.id).collect(),
        configuration_revision: before.options.configuration.revision,
        donbot_revision: before.options.donbot.revision,
    };
    h.handle.submit_aggregate(command.clone()).unwrap();
    assert_eq!(
        h.handle.submit_aggregate(command),
        Err(DonBotAggregateDeliveryError::Busy)
    );
    assert!(h.events.lock().unwrap().aggregate.is_empty());
    assert!(h.events.lock().unwrap().launched.is_empty());
    h.tick();
    assert_eq!(h.events.lock().unwrap().aggregate.len(), 1);
    assert_eq!(
        h.events.lock().unwrap().launched,
        ["https://dps.report/fixture"]
    );
}

#[test]
fn cancellation_closes_admission_cancels_owned_ports_and_persists_final_state_once() {
    let mut h = Harness::standard();
    h.admit();
    h.parse();
    h.tick();
    h.handle
        .submit_options(Command::SetWindowVisible(false))
        .unwrap();
    h.stop.request_stop();
    assert_eq!(
        h.runtime.tick(&h.stop.token()),
        Err(RuntimeError::ShuttingDown)
    );
    let snapshot = h.handle.snapshot();
    assert!(snapshot.shutting_down);
    assert_eq!(
        snapshot.jobs[0].record.providers[0].state,
        ProviderState::Cancelled
    );
    assert!(
        snapshot
            .options
            .configuration
            .settings
            .general
            .window_visible
    );
    let cancellations = h.events.lock().unwrap().cancellations;
    let writes = h.events.lock().unwrap().history_writes;
    h.runtime.shutdown();
    assert_eq!(h.events.lock().unwrap().cancellations, cancellations);
    assert_eq!(h.events.lock().unwrap().history_writes, writes);
    assert_eq!(
        h.handle.submit_options(Command::ConnectTwitch),
        Err(NexusOptionsError::ShuttingDown)
    );
    assert_eq!(
        h.handle.submit_action(RecentLogAction::DismissError),
        Err(RecentLogActionError::ShuttingDown)
    );
    assert!(
        !h.events
            .lock()
            .unwrap()
            .secret_operations
            .contains(&"erase")
    );
}
