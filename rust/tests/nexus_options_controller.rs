use manny_uploader::{application::*, domain::*, providers::TwitchClient};
use std::{
    collections::VecDeque,
    path::Path,
    sync::{Arc, Mutex},
    time::{Duration, Instant, SystemTime},
};
type Command = NexusOptionsCommand;

fn secret() -> SecretValue {
    SecretValue::from_bytes(b"SYNTHETIC-PRIVATE-CREDENTIAL".to_vec())
}
#[derive(Default)]
struct Events {
    saves: Vec<Settings>,
    secret_events: Vec<&'static str>,
    fail_save: bool,
    panic_save: bool,
    verify: VecDeque<DonBotVerificationRequest>,
    verified: VecDeque<DonBotVerificationResult>,
    auth: VecDeque<TwitchAuthenticationRequest>,
    authenticated: VecDeque<TwitchAuthenticationResult>,
    tests: VecDeque<TwitchTestMessageRequest>,
    tested: VecDeque<TwitchTestMessageResult>,
    test_cancellations: usize,
    urls: Vec<String>,
    fail_launch: bool,
    panic_launch: bool,
    on_launch: Option<NexusOptionsHandle>,
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
    fn save(&self, settings: &Settings) -> Result<(), SettingsStoreError> {
        let mut events = self.0.lock().unwrap();
        if events.panic_save {
            drop(events);
            panic!("synthetic adapter failure");
        }
        if events.fail_save {
            return Err(SettingsStoreError::SerializationFailed);
        }
        events.saves.push(settings.clone());
        Ok(())
    }
}
struct Secrets(Arc<Mutex<Events>>);
impl SecretStore for Secrets {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::NativeUserScoped
    }
    fn load(&self, _: SecretId) -> Result<SecretValue, SecretStoreError> {
        self.0.lock().unwrap().secret_events.push("load");
        Ok(secret())
    }
    fn store(&mut self, _: SecretId, _: &SecretValue) -> Result<(), SecretStoreError> {
        self.0.lock().unwrap().secret_events.push("store");
        Ok(())
    }
    fn erase(&mut self, _: SecretId) -> Result<(), SecretStoreError> {
        self.0.lock().unwrap().secret_events.push("erase");
        Ok(())
    }
}
struct Verifier(Arc<Mutex<Events>>);
impl DonBotVerifier for Verifier {
    fn enqueue(
        &mut self,
        request: DonBotVerificationRequest,
    ) -> Result<(), DonBotVerificationDispatchError> {
        self.0.lock().unwrap().verify.push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<DonBotVerificationResult> {
        self.0.lock().unwrap().verified.pop_front()
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().verify.clear();
    }
}
struct Authenticator(Arc<Mutex<Events>>);
impl TwitchAuthenticator for Authenticator {
    fn enqueue(
        &mut self,
        request: TwitchAuthenticationRequest,
    ) -> Result<(), TwitchAuthenticationDispatchError> {
        self.0.lock().unwrap().auth.push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<TwitchAuthenticationResult> {
        self.0.lock().unwrap().authenticated.pop_front()
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().auth.clear();
    }
}
struct Messenger(Arc<Mutex<Events>>);
impl TwitchTestMessenger for Messenger {
    fn enqueue(
        &mut self,
        request: TwitchTestMessageRequest,
    ) -> Result<(), TwitchTestMessageDispatchError> {
        self.0.lock().unwrap().tests.push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<TwitchTestMessageResult> {
        self.0.lock().unwrap().tested.pop_front()
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().test_cancellations += 1;
    }
}
struct Launcher(Arc<Mutex<Events>>);
impl ExternalActionLauncher for Launcher {
    fn open_url(&mut self, url: &str) -> Result<(), ExternalActionError> {
        let mut events = self.0.lock().unwrap();
        events.urls.push(url.into());
        if let Some(handle) = events.on_launch.take() {
            let snapshot = handle.snapshot();
            assert_eq!(snapshot.pending_commands, 0);
            handle.submit(Command::SetWindowVisible(false)).unwrap();
        }
        if events.panic_launch {
            drop(events);
            panic!("synthetic launcher failure");
        }
        if events.fail_launch {
            Err(ExternalActionError::LaunchFailed)
        } else {
            Ok(())
        }
    }
    fn open_directory(&mut self, _: &Path) -> Result<(), ExternalActionError> {
        panic!("no directory command")
    }
}
struct NoHttp;
impl HttpClient for NoHttp {
    fn execute(&self, _: HttpRequest, _: &StopToken) -> Result<HttpResponse, HttpError> {
        panic!("no HTTP from options")
    }
}
struct TestClock(Mutex<Instant>);
impl Clock for TestClock {
    fn steady_now(&self) -> Instant {
        *self.0.lock().unwrap()
    }
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH + Duration::from_secs(10000)
    }
}
struct Harness {
    events: Arc<Mutex<Events>>,
    clock: Arc<TestClock>,
    config: Arc<Mutex<ConfigurationService>>,
    controller: NexusOptionsController,
    handle: NexusOptionsHandle,
}
impl Harness {
    fn new(options: NexusOptionsControllerConfig) -> Self {
        Self::create(options).unwrap()
    }
    fn create(options: NexusOptionsControllerConfig) -> Result<Self, NexusOptionsError> {
        let events = Arc::new(Mutex::new(Events::default()));
        let config = Arc::new(Mutex::new(
            ConfigurationService::new(
                Box::new(Store(
                    events.clone(),
                    Settings::new("synthetic-logs".into()),
                )),
                Ok(Box::new(Secrets(events.clone()))),
            )
            .unwrap(),
        ));
        let clock = Arc::new(TestClock(Mutex::new(Instant::now())));
        let api = Arc::new(TwitchClient::new(Arc::new(NoHttp), "publicclient".into()).unwrap());
        let owner = Arc::new(TwitchSessionOwner::new(config.clone(), api, clock.clone()));
        let twitch = TwitchAuthenticationController::new(
            config.clone(),
            Box::new(Authenticator(events.clone())),
            owner,
            clock.clone(),
        )
        .unwrap();
        let donbot = DonBotConfigurationController::new(
            &config.lock().unwrap(),
            Box::new(Verifier(events.clone())),
        )
        .unwrap();
        let controller = NexusOptionsController::new(
            config.clone(),
            donbot,
            twitch,
            TwitchTestMessageController::new(Box::new(Messenger(events.clone()))),
            Box::new(Launcher(events.clone())),
            options,
        )?;
        let handle = controller.handle();
        Ok(Self {
            events,
            clock,
            config,
            controller,
            handle,
        })
    }
    fn tick(&mut self) -> NexusOptionsTickReport {
        self.controller.tick().unwrap()
    }
    fn run(&mut self, command: Command) -> NexusOptionsTickReport {
        self.handle.submit(command).unwrap();
        self.tick()
    }
    fn auth_reply(
        &self,
        make: impl FnOnce(TwitchAuthenticationCommand) -> TwitchAuthenticationSuccess,
    ) {
        let mut events = self.events.lock().unwrap();
        let request = events.auth.pop_front().unwrap();
        events.authenticated.push_back(TwitchAuthenticationResult {
            request_id: request.request_id,
            operation: request.command.operation(),
            outcome: Ok(make(request.command)),
            retained: None,
        });
    }
    fn authorize(&mut self) {
        self.run(Command::ConnectTwitch);
        self.auth_reply(|_| {
            TwitchAuthenticationSuccess::Started(TwitchDeviceAuthorization {
                device_code: secret(),
                user_code: "SYNTHETIC-USER-CODE".into(),
                verification_uri: "https://www.twitch.tv/activate?code=SYNTHETIC-USER-CODE".into(),
                expires_in: Duration::from_secs(600),
                polling_interval: Duration::from_secs(5),
            })
        });
        self.tick();
    }
    fn connect(&mut self) {
        self.authorize();
        *self.clock.0.lock().unwrap() += Duration::from_secs(5);
        self.tick();
        self.auth_reply(|_| {
            TwitchAuthenticationSuccess::Granted(TwitchTokenGrant {
                access_token: secret(),
                refresh_token: secret(),
                expires_in: Duration::from_secs(14400),
                scopes: vec![TWITCH_CHAT_SCOPE.into()],
            })
        });
        self.tick();
        self.auth_reply(|command| {
            let TwitchAuthenticationCommand::Validate { credentials } = command else {
                panic!("validation expected")
            };
            TwitchAuthenticationSuccess::Validated {
                credentials,
                identity: TwitchValidatedIdentity {
                    user_id: "123".into(),
                    login: "broadcaster".into(),
                    scopes: vec![TWITCH_CHAT_SCOPE.into()],
                    expires_in: Duration::from_secs(14400),
                },
            }
        });
        self.tick();
        assert_eq!(
            self.handle.snapshot().twitch.state,
            TwitchConnectionState::Connected
        );
    }
    fn verify_reply(&self) {
        let mut events = self.events.lock().unwrap();
        let request = events.verify.pop_front().unwrap();
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
                            ..Default::default()
                        },
                    }],
                },
            }),
        });
    }
}

#[test]
fn submissions_do_no_io_and_fifo_ticks_are_bounded() {
    let mut h = Harness::new(NexusOptionsControllerConfig {
        command_capacity: 4,
        max_commands_per_tick: 2,
    });
    h.handle.submit(Command::SetWindowVisible(false)).unwrap();
    h.handle.submit(Command::SetWingmanEnabled(false)).unwrap();
    h.handle
        .submit(Command::VerifyDonBot {
            api_base_url: DEFAULT_DONBOT_API.into(),
            api_key: secret(),
        })
        .unwrap();
    h.handle.submit(Command::ConnectTwitch).unwrap();
    {
        let events = h.events.lock().unwrap();
        assert!(events.saves.is_empty() && events.secret_events.is_empty());
        assert!(events.verify.is_empty() && events.auth.is_empty());
    }
    assert_eq!(h.tick().commands_processed, 2);
    assert_eq!(h.handle.snapshot().pending_commands, 2);
    {
        let events = h.events.lock().unwrap();
        assert!(!events.saves[0].general.window_visible);
        assert!(events.saves[0].wingman.enabled);
        assert!(!events.saves[1].wingman.enabled);
        assert!(events.verify.is_empty() && events.auth.is_empty());
    }
    assert_eq!(h.tick().commands_processed, 2);
    assert_eq!(h.events.lock().unwrap().verify.len(), 1);
    assert_eq!(h.events.lock().unwrap().auth.len(), 1);
}

#[test]
fn queue_limits_invalid_inputs_and_rejection_snapshots() {
    for (capacity, tick) in [(0, 1), (257, 8), (8, 0), (8, 9)] {
        assert!(matches!(
            Harness::create(NexusOptionsControllerConfig {
                command_capacity: capacity,
                max_commands_per_tick: tick
            }),
            Err(NexusOptionsError::InvalidConfiguration)
        ));
    }
    let h = Harness::new(NexusOptionsControllerConfig {
        command_capacity: 1,
        max_commands_per_tick: 1,
    });
    for command in [
        Command::SelectDonBotGuild("0123".into()),
        Command::SelectDonBotDiscordChannel("bad".into()),
        Command::OpenTwitchAuthorization { revision: 0 },
        Command::VerifyDonBot {
            api_base_url: "http://invalid".into(),
            api_key: secret(),
        },
    ] {
        assert_eq!(
            h.handle.submit(command),
            Err(NexusOptionsError::InvalidCommand)
        );
        assert_eq!(
            h.handle.snapshot().last_error,
            Some(NexusOptionsError::InvalidCommand)
        );
        assert_eq!(h.handle.snapshot().pending_commands, 0);
    }
    let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().configuration.settings);
    draft.general.log_directory.clear();
    assert!(matches!(
        h.handle.submit(Command::SaveOrdinary(draft)),
        Err(NexusOptionsError::InvalidSettings(_))
    ));
    h.handle.submit(Command::DismissError).unwrap();
    assert_eq!(
        h.handle.submit(Command::DismissError),
        Err(NexusOptionsError::QueueFull)
    );
}

#[test]
fn narrow_changes_and_stale_drafts_preserve_destinations() {
    let mut h = Harness::new(Default::default());
    let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().configuration.settings);
    draft.general.poll_interval_ms = 2500;
    h.handle
        .submit(Command::SetDpsReportEnabled(false))
        .unwrap();
    h.handle
        .submit(Command::SetDpsReportDetailedWvw(true))
        .unwrap();
    h.handle.submit(Command::SetWingmanEnabled(false)).unwrap();
    h.handle.submit(Command::SaveOrdinary(draft)).unwrap();
    assert_eq!(h.tick().action_failures, 0);
    let before = h.handle.snapshot().configuration.settings;
    assert!(!before.dps_report.enabled && !before.wingman.enabled);
    assert!(before.dps_report.detailed_wvw);
    assert_eq!(before.general.poll_interval_ms, 2500);
    h.run(Command::SetWindowVisible(false));
    let mut expected = before;
    expected.general.window_visible = false;
    assert_eq!(h.handle.snapshot().configuration.settings, expected);
}

#[test]
fn stale_ordinary_save_preserves_queued_visibility_changes() {
    for visible in [false, true] {
        for max_commands_per_tick in [1, 8] {
            let mut h = Harness::new(NexusOptionsControllerConfig {
                command_capacity: 8,
                max_commands_per_tick,
            });
            h.run(Command::SetWindowVisible(!visible));
            let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().configuration.settings);
            draft.general.poll_interval_ms = 2500;
            h.handle.submit(Command::SetWindowVisible(visible)).unwrap();
            h.handle.submit(Command::SaveOrdinary(draft)).unwrap();
            while h.handle.snapshot().pending_commands != 0 {
                assert_eq!(h.tick().action_failures, 0);
            }
            let settings = h.handle.snapshot().configuration.settings;
            assert_eq!(settings.general.window_visible, visible);
            assert_eq!(settings.general.poll_interval_ms, 2500);
            assert_eq!(h.events.lock().unwrap().saves.last(), Some(&settings));
        }
    }
}

#[test]
fn failed_saves_leave_published_settings_unchanged_and_can_be_dismissed() {
    let mut h = Harness::new(Default::default());
    let before = h.handle.snapshot().configuration;
    h.events.lock().unwrap().fail_save = true;
    assert_eq!(h.run(Command::SetWindowVisible(false)).action_failures, 1);
    assert_eq!(h.handle.snapshot().configuration, before);
    assert!(matches!(
        h.handle.snapshot().last_error,
        Some(NexusOptionsError::Configuration(_))
    ));
    h.run(Command::DismissError);
    assert_eq!(h.handle.snapshot().last_error, None);
}

#[test]
fn donbot_verify_select_enable_refresh_and_disconnect() {
    let mut h = Harness::new(Default::default());
    assert_eq!(h.run(Command::SetDonBotEnabled(true)).action_failures, 1);
    h.run(Command::VerifyDonBot {
        api_base_url: DEFAULT_DONBOT_API.into(),
        api_key: secret(),
    });
    h.verify_reply();
    assert!(h.tick().donbot_progressed);
    assert_eq!(h.events.lock().unwrap().secret_events, ["store"]);
    assert_eq!(
        h.run(Command::SelectDonBotGuild("999".into()))
            .action_failures,
        1
    );
    h.run(Command::SelectDonBotGuild("123".into()));
    h.run(Command::SetDonBotEnabled(true));
    assert!(h.handle.snapshot().donbot.refreshing);
    assert_eq!(h.events.lock().unwrap().verify.len(), 1);
    assert_eq!(h.run(Command::SetDonBotEnabled(false)).action_failures, 1);
    assert!(h.handle.snapshot().configuration.settings.donbot.enabled);
    h.verify_reply();
    h.tick();
    h.run(Command::SetDonBotDiscordDeliveryEnabled(true));
    assert!(
        h.handle
            .snapshot()
            .configuration
            .settings
            .donbot
            .discord_delivery_enabled
    );
    h.run(Command::SetDonBotEnabled(false));
    assert!(
        !h.handle
            .snapshot()
            .configuration
            .settings
            .donbot
            .discord_delivery_enabled
    );
    h.run(Command::DisconnectDonBot);
    assert_eq!(
        h.handle.snapshot().donbot.state,
        DonBotConfigurationState::Unverified
    );
    assert_eq!(
        h.events.lock().unwrap().secret_events.last(),
        Some(&"erase")
    );
    assert!(!format!("{:?}", h.handle.snapshot()).contains("SYNTHETIC-PRIVATE-CREDENTIAL"));
}

#[test]
fn twitch_connect_posting_dependency_client_id_and_disconnect() {
    let mut h = Harness::new(Default::default());
    assert_eq!(h.run(Command::SetTwitchEnabled(true)).action_failures, 1);
    h.connect();
    h.run(Command::SetTwitchEnabled(true));
    assert!(h.handle.snapshot().configuration.settings.twitch.enabled);
    let mut draft = NexusOrdinaryOptions::from(&h.handle.snapshot().configuration.settings);
    draft.twitch_client_id = "replacement".into();
    assert_eq!(h.run(Command::SaveOrdinary(draft)).action_failures, 1);
    assert_eq!(
        h.handle.snapshot().last_error,
        Some(NexusOptionsError::ClientIdInUse)
    );
    let secrets_before = h.events.lock().unwrap().secret_events.len();
    h.run(Command::SetDpsReportEnabled(false));
    assert!(!h.handle.snapshot().configuration.settings.twitch.enabled);
    assert_eq!(
        h.handle.snapshot().twitch.state,
        TwitchConnectionState::Connected
    );
    assert_eq!(h.events.lock().unwrap().secret_events.len(), secrets_before);
    assert_eq!(h.run(Command::SetTwitchEnabled(true)).action_failures, 1);
    h.run(Command::DisconnectTwitch);
    h.auth_reply(|_| TwitchAuthenticationSuccess::Revoked);
    h.tick();
    assert_eq!(
        h.handle.snapshot().twitch.state,
        TwitchConnectionState::Disconnected
    );
    assert_eq!(
        h.events.lock().unwrap().secret_events.last(),
        Some(&"erase")
    );
}

#[test]
fn authorization_launch_resolves_current_revision_and_runs_without_queue_lock() {
    let mut h = Harness::new(Default::default());
    h.authorize();
    assert_eq!(h.events.lock().unwrap().urls.len(), 1);
    h.tick();
    assert_eq!(h.events.lock().unwrap().urls.len(), 1);
    h.events.lock().unwrap().urls.clear();
    let revision = h.handle.snapshot().twitch.revision;
    h.handle
        .submit(Command::OpenTwitchAuthorization { revision })
        .unwrap();
    assert!(h.events.lock().unwrap().urls.is_empty());
    h.events.lock().unwrap().on_launch = Some(h.handle.clone());
    let before = h.handle.snapshot().revision;
    assert_eq!(h.tick().commands_processed, 1);
    assert!(h.handle.snapshot().revision > before);
    assert_eq!(h.handle.snapshot().pending_commands, 1);
    assert!(
        h.handle
            .snapshot()
            .configuration
            .settings
            .general
            .window_visible
    );
    h.tick();
    assert!(
        !h.handle
            .snapshot()
            .configuration
            .settings
            .general
            .window_visible
    );
    assert_eq!(
        h.run(Command::OpenTwitchAuthorization {
            revision: revision - 1
        })
        .action_failures,
        1
    );
    assert_eq!(h.events.lock().unwrap().urls.len(), 1);
    for panic in [false, true] {
        h.events.lock().unwrap().fail_launch = !panic;
        h.events.lock().unwrap().panic_launch = panic;
        h.run(Command::OpenTwitchAuthorization { revision });
        assert_eq!(
            h.handle.snapshot().last_error,
            Some(NexusOptionsError::BrowserLaunchFailed)
        );
        assert!(!format!("{:?}", h.handle.snapshot().last_error).contains("SYNTHETIC-USER-CODE"));
    }
}

#[test]
fn connect_opens_authorization_once_and_reports_browser_failures() {
    for panic in [false, true] {
        let mut h = Harness::new(Default::default());
        h.events.lock().unwrap().fail_launch = !panic;
        h.events.lock().unwrap().panic_launch = panic;
        h.authorize();
        assert_eq!(h.events.lock().unwrap().urls.len(), 1);
        assert_eq!(
            h.handle.snapshot().last_error,
            Some(NexusOptionsError::BrowserLaunchFailed)
        );
        h.tick();
        assert_eq!(h.events.lock().unwrap().urls.len(), 1);
    }
}

#[test]
fn authorization_uri_origin_bounds_and_delimiters() {
    for uri in [
        "https://www.twitch.tv/activate",
        "https://www.twitch.tv/activate?public-code=123",
    ] {
        assert!(valid_twitch_authorization_uri(uri));
    }
    for uri in [
        "http://www.twitch.tv/activate",
        "https://www.twitch.tv/activate.evil",
        "https://www.twitch.tv/activate/",
        "https://www.twitch.tv/activate#fragment",
        "https://www.twitch.tv/activate?x=@evil",
        "https://www.twitch.tv/activate?x=\"",
        "https://www.twitch.tv/activate?x=\n",
        "https://www.twitch.tv/activate?x=\\",
    ] {
        assert!(!valid_twitch_authorization_uri(uri));
    }
    let prefix = "https://www.twitch.tv/activate?";
    assert!(valid_twitch_authorization_uri(&format!(
        "{prefix}{}",
        "x".repeat(2048 - prefix.len())
    )));
    assert!(!valid_twitch_authorization_uri(&format!(
        "{prefix}{}",
        "x".repeat(2049 - prefix.len())
    )));
}

#[test]
fn test_message_is_explicit_correlated_and_never_retried() {
    let mut h = Harness::new(Default::default());
    assert_eq!(h.run(Command::SendTwitchTestMessage).action_failures, 1);
    h.connect();
    h.run(Command::SendTwitchTestMessage);
    assert_eq!(h.run(Command::SendTwitchTestMessage).action_failures, 1);
    let request = h.events.lock().unwrap().tests.pop_front().unwrap();
    h.events
        .lock()
        .unwrap()
        .tested
        .push_back(TwitchTestMessageResult {
            request_id: request.request_id,
            outcome: TwitchTestMessageOutcome::Ambiguous,
        });
    h.tick();
    assert_eq!(
        h.handle.snapshot().twitch_test_message.diagnostic,
        Some(TwitchTestMessageError::Ambiguous)
    );
    for _ in 0..3 {
        h.tick();
    }
    assert!(h.events.lock().unwrap().tests.is_empty());
    h.run(Command::SendTwitchTestMessage);
    assert_eq!(h.events.lock().unwrap().tests.len(), 1);
}

#[test]
fn concurrent_submissions_are_bounded_and_shutdown_drops_queued_credentials() {
    let mut h = Harness::new(Default::default());
    let results = std::thread::scope(|scope| {
        let threads: Vec<_> = (0..40)
            .map(|_| {
                let handle = h.handle.clone();
                scope.spawn(move || {
                    handle.submit(Command::VerifyDonBot {
                        api_base_url: DEFAULT_DONBOT_API.into(),
                        api_key: secret(),
                    })
                })
            })
            .collect();
        threads
            .into_iter()
            .map(|thread| thread.join().unwrap())
            .collect::<Vec<_>>()
    });
    assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 32);
    assert_eq!(h.handle.snapshot().pending_commands, 32);
    h.controller.shutdown();
    h.controller.shutdown();
    assert_eq!(h.events.lock().unwrap().test_cancellations, 1);
    assert!(h.events.lock().unwrap().verify.is_empty());
    assert_eq!(h.handle.snapshot().pending_commands, 0);
    assert!(h.handle.snapshot().shutting_down);
    assert_eq!(
        h.handle.submit(Command::ConnectTwitch),
        Err(NexusOptionsError::ShuttingDown)
    );
    assert_eq!(h.controller.tick(), Err(NexusOptionsError::ShuttingDown));
}

#[test]
fn stopping_admission_is_immediate_and_owner_cancels_test_work_on_next_tick() {
    let mut h = Harness::new(Default::default());
    h.connect();
    h.run(Command::SendTwitchTestMessage);
    h.handle.submit(Command::SetWindowVisible(false)).unwrap();
    h.handle.stop_accepting();
    assert_eq!(h.handle.snapshot().pending_commands, 0);
    assert!(!h.handle.snapshot().accepting_commands);
    assert_eq!(h.controller.tick(), Err(NexusOptionsError::ShuttingDown));
    assert_eq!(h.events.lock().unwrap().test_cancellations, 1);
    assert!(
        h.handle
            .snapshot()
            .configuration
            .settings
            .general
            .window_visible
    );
}

#[test]
fn idle_ticks_do_not_change_revision_and_external_config_changes_publish() {
    let mut h = Harness::new(Default::default());
    let before = h.handle.snapshot();
    h.tick();
    assert_eq!(h.handle.snapshot(), before);
    let mut settings = before.configuration.settings;
    settings.general.poll_interval_ms = 2500;
    h.config.lock().unwrap().save_settings(settings).unwrap();
    h.tick();
    assert_eq!(
        h.handle
            .snapshot()
            .configuration
            .settings
            .general
            .poll_interval_ms,
        2500
    );
    assert!(h.handle.snapshot().revision > before.revision);
}

#[test]
fn storage_panics_publish_safe_error_without_unwinding_application_owner() {
    let mut h = Harness::new(Default::default());
    h.events.lock().unwrap().panic_save = true;
    h.handle.submit(Command::SetWindowVisible(false)).unwrap();
    let result = h.controller.tick();
    assert!(result.is_ok());
    assert_eq!(
        h.handle.snapshot().last_error,
        Some(NexusOptionsError::Internal)
    );
    assert!(
        h.handle
            .snapshot()
            .configuration
            .settings
            .general
            .window_visible
    );
}
