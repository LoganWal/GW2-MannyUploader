use manny_uploader::{application::*, domain::Settings};
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
};

const BASE: &str = "https://donbot.invalid";
const KEY: &[u8] = b"synthetic-candidate-key";
type Error = DonBotConfigurationError;
type State = DonBotConfigurationState;
#[derive(Default)]
struct Storage {
    events: Vec<&'static str>,
    saved: Vec<Settings>,
    fail_save: bool,
    fail_secret: bool,
    panic_save: bool,
}
struct Ordinary(Arc<Mutex<Storage>>, Settings);
impl SettingsStore for Ordinary {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        Ok(SettingsLoadResult {
            settings: self.1.clone(),
            source: SettingsLoadSource::Primary,
            recovery: None,
        })
    }
    fn save(&self, settings: &Settings) -> Result<(), SettingsStoreError> {
        let mut state = self.0.lock().unwrap();
        state.events.push("save");
        if state.panic_save {
            drop(state);
            panic!("synthetic adapter failure");
        }
        if state.fail_save {
            return Err(SettingsStoreError::InvalidValues(vec![]));
        }
        state.saved.push(settings.clone());
        Ok(())
    }
}
struct Protected(Arc<Mutex<Storage>>);
impl Protected {
    fn operation(&self, name: &'static str) -> Result<(), SecretStoreError> {
        let mut state = self.0.lock().unwrap();
        state.events.push(name);
        if state.fail_secret {
            return Err(secret_error());
        }
        Ok(())
    }
}
fn secret_error() -> SecretStoreError {
    SecretStoreError {
        id: Some(SecretId::DonBotGw2ApiKey),
        code: SecretStoreErrorCode::NotFound,
        system_error: None,
    }
}
impl SecretStore for Protected {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::NativeUserScoped
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        assert_eq!(id, SecretId::DonBotGw2ApiKey);
        self.operation("load")?;
        Ok(key())
    }
    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::DonBotGw2ApiKey);
        assert_eq!(value.expose(), KEY);
        self.operation("store")
    }
    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::DonBotGw2ApiKey);
        self.operation("erase")
    }
}
#[derive(Default)]
struct Verification {
    requests: VecDeque<DonBotVerificationRequest>,
    results: VecDeque<DonBotVerificationResult>,
    reject: bool,
    panic_enqueue: bool,
    panic_poll: bool,
    cancellations: usize,
}
struct Verifier(Arc<Mutex<Verification>>);
impl DonBotVerifier for Verifier {
    fn enqueue(
        &mut self,
        request: DonBotVerificationRequest,
    ) -> Result<(), DonBotVerificationDispatchError> {
        let mut state = self.0.lock().unwrap();
        if state.panic_enqueue {
            drop(state);
            panic!("synthetic verifier failure");
        }
        if state.reject {
            return Err(DonBotVerificationDispatchError::QueueFull);
        }
        state.requests.push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<DonBotVerificationResult> {
        let mut state = self.0.lock().unwrap();
        if state.panic_poll {
            drop(state);
            panic!("synthetic poll failure");
        }
        state.results.pop_front()
    }
    fn cancel_pending(&mut self) {
        let mut state = self.0.lock().unwrap();
        state.cancellations += 1;
        state.requests.clear();
        state.results.clear();
    }
}
struct Harness {
    storage: Arc<Mutex<Storage>>,
    verification: Arc<Mutex<Verification>>,
    configuration: ConfigurationService,
    controller: DonBotConfigurationController,
}
impl Harness {
    fn new(settings: Settings) -> Self {
        let storage = Arc::new(Mutex::new(Storage::default()));
        let verification = Arc::new(Mutex::new(Verification::default()));
        let configuration = ConfigurationService::new(
            Box::new(Ordinary(storage.clone(), settings)),
            Ok(Box::new(Protected(storage.clone()))),
        )
        .unwrap();
        let controller = DonBotConfigurationController::new(
            &configuration,
            Box::new(Verifier(verification.clone())),
        )
        .unwrap();
        Self {
            storage,
            verification,
            configuration,
            controller,
        }
    }
    fn begin(&mut self) {
        self.controller
            .begin_verification(&self.configuration, format!("{BASE}///"), key())
            .unwrap();
    }
    fn reply(&self, identity: Result<DonBotVerification, DonBotVerificationFailure>) {
        let mut verifier = self.verification.lock().unwrap();
        let request = verifier.requests.pop_front().unwrap();
        verifier.results.push_back(DonBotVerificationResult {
            request_id: request.request_id,
            verification: identity.map(|identity| DonBotVerificationSuccess {
                identity,
                api_key: request.api_key,
            }),
        });
    }
    fn poll(&mut self) -> Result<bool, Error> {
        self.controller.poll(&mut self.configuration)
    }
    fn verified(&mut self) {
        self.controller
            .begin_saved_verification(&self.configuration)
            .unwrap();
        self.reply(Ok(identity()));
        assert_eq!(self.poll(), Ok(true));
        self.storage.lock().unwrap().events.clear();
    }
    fn settings(&self) -> Settings {
        self.configuration.snapshot().settings
    }
}
fn key() -> SecretValue {
    SecretValue::from_bytes(KEY.to_vec())
}
fn settings() -> Settings {
    let mut settings = Settings::new("synthetic/logs".into());
    settings.donbot.api_base_url = BASE.into();
    settings.donbot.selected_guild_id = "123".into();
    settings.donbot.enabled = true;
    settings.donbot.discord_delivery_enabled = true;
    settings.donbot.discord_channel_override_explicit = true;
    settings.donbot.selected_discord_channel_id = "456".into();
    settings
}
fn identity() -> DonBotVerification {
    DonBotVerification {
        account_name: "Synthetic.1234".into(),
        discord_summary_delivery_v1: true,
        discord_aggregate_delivery_v1: true,
        guilds: ["123", "789"]
            .into_iter()
            .map(|id| DonBotGuild {
                guild_id: id.into(),
                guild_name: "Synthetic guild".into(),
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
                        channel_name: "Synthetic channel".into(),
                    }],
                    ..Default::default()
                },
            })
            .collect(),
    }
}
fn assert_disabled(settings: &Settings) {
    assert!(!settings.donbot.enabled);
    assert!(settings.donbot.selected_guild_id.is_empty());
    assert!(!settings.donbot.discord_delivery_enabled);
    assert!(!settings.donbot.discord_channel_override_explicit);
    assert!(settings.donbot.selected_discord_channel_id.is_empty());
}

#[test]
fn candidate_is_moved_normalized_and_saved_before_protected_replacement() {
    let mut h = Harness::new(settings());
    let candidate = key();
    let allocation = candidate.expose().as_ptr();
    h.controller
        .begin_verification(&h.configuration, format!("{BASE}///"), candidate)
        .unwrap();
    assert!(h.storage.lock().unwrap().events.is_empty());
    let verifier = h.verification.lock().unwrap();
    assert_eq!(verifier.requests[0].api_base_url, BASE);
    assert_eq!(verifier.requests[0].api_key.expose().as_ptr(), allocation);
    drop(verifier);
    assert_eq!(h.controller.snapshot().state, State::Verifying);
    h.reply(Ok(identity()));
    assert_eq!(h.poll(), Ok(true));
    assert_eq!(h.storage.lock().unwrap().events, ["save", "store"]);
    assert_disabled(&h.settings());
    let snapshot = h.controller.snapshot();
    assert_eq!(snapshot.state, State::Verified);
    assert_eq!(snapshot.account_name.as_deref(), Some("Synthetic.1234"));
    assert!(!format!("{snapshot:?}").contains(std::str::from_utf8(KEY).unwrap()));
    let mut detached = snapshot.clone();
    detached.guilds.clear();
    assert_eq!(h.controller.snapshot(), snapshot);
    assert_eq!(h.poll(), Ok(false));
}

#[test]
fn candidate_failure_and_storage_failures_never_publish_verified_identity() {
    for failure in [
        DonBotVerificationFailure::Failed,
        DonBotVerificationFailure::Cancelled,
    ] {
        let mut h = Harness::new(settings());
        h.begin();
        h.reply(Err(failure));
        assert!(h.poll().is_err());
        assert_eq!(h.settings(), settings());
        assert!(h.storage.lock().unwrap().events.is_empty());
        assert_eq!(h.controller.snapshot().state, State::Error);
    }
    for fail_save in [true, false] {
        let mut h = Harness::new(settings());
        h.begin();
        h.reply(Ok(identity()));
        h.storage.lock().unwrap().fail_save = fail_save;
        h.storage.lock().unwrap().fail_secret = !fail_save;
        assert_eq!(
            h.poll(),
            Err(if fail_save {
                Error::SettingsSaveFailed
            } else {
                Error::SecretStoreFailed
            })
        );
        assert_eq!(h.controller.snapshot().state, State::Error);
        if fail_save {
            assert_eq!(h.storage.lock().unwrap().events, ["save"]);
            assert_eq!(h.settings(), settings());
        } else {
            assert_eq!(h.storage.lock().unwrap().events, ["save", "store"]);
            assert_disabled(&h.settings());
        }
    }
}

#[test]
fn stale_results_do_not_finish_active_request_and_requests_increase() {
    let mut h = Harness::new(settings());
    h.begin();
    h.verification
        .lock()
        .unwrap()
        .results
        .push_back(DonBotVerificationResult {
            request_id: 99,
            verification: Err(DonBotVerificationFailure::Failed),
        });
    let before = h.controller.snapshot();
    assert_eq!(h.poll(), Err(Error::StaleVerification));
    assert_eq!(h.controller.snapshot(), before);
    assert_eq!(
        h.controller.disconnect(&mut h.configuration),
        Err(Error::Busy)
    );
    assert_eq!(
        h.controller.begin_saved_verification(&h.configuration),
        Err(Error::Busy)
    );
    assert!(h.storage.lock().unwrap().events.is_empty());
    h.reply(Err(DonBotVerificationFailure::Failed));
    assert!(h.poll().is_err());
    h.begin();
    assert_eq!(h.verification.lock().unwrap().requests[0].request_id, 2);
}

#[test]
fn changed_base_invalidates_saved_and_candidate_success_without_writes() {
    for candidate in [true, false] {
        let mut h = Harness::new(settings());
        if candidate {
            h.begin();
        } else {
            h.controller
                .begin_saved_verification(&h.configuration)
                .unwrap();
        }
        let mut changed = h.settings();
        changed.donbot.api_base_url = "https://other.invalid".into();
        h.configuration.save_settings(changed.clone()).unwrap();
        h.storage.lock().unwrap().events.clear();
        h.reply(Ok(identity()));
        assert_eq!(h.poll(), Err(Error::StaleVerification));
        assert_eq!(h.settings(), changed);
        assert!(h.storage.lock().unwrap().events.is_empty());
        assert_eq!(
            authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
            DonBotDeliveryRoute::None
        );
    }
}

#[test]
fn startup_preserves_authorized_settings_without_rewriting_key() {
    let mut h = Harness::new(settings());
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::None
    );
    h.controller
        .begin_saved_verification(&h.configuration)
        .unwrap();
    h.reply(Ok(identity()));
    assert_eq!(h.poll(), Ok(true));
    assert_eq!(h.storage.lock().unwrap().events, ["load"]);
    assert_eq!(h.settings(), settings());
    assert_eq!(h.controller.snapshot().selected_guild_id, "123");
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::ChannelOverride {
            channel_id: "456".into()
        }
    );
}

#[test]
fn revoked_guild_or_route_is_persisted_before_publishing() {
    for revoke in 0..5 {
        let mut h = Harness::new(settings());
        h.controller
            .begin_saved_verification(&h.configuration)
            .unwrap();
        let mut revoked = identity();
        match revoke {
            0 => revoked.guilds.clear(),
            1 => revoked.discord_summary_delivery_v1 = false,
            2 => revoked.guilds[0].discord_delivery.enabled = false,
            3 => revoked.guilds[0].discord_delivery.channels.clear(),
            4 => revoked.guilds[0].discord_delivery.channel_override_allowed = false,
            _ => unreachable!(),
        }
        h.reply(Ok(revoked));
        assert_eq!(h.poll(), Ok(true));
        assert_eq!(h.storage.lock().unwrap().events, ["load", "save"]);
        let saved = h.settings();
        assert_eq!(saved.donbot.enabled, revoke != 0);
        assert!(!saved.donbot.discord_delivery_enabled);
        assert!(!saved.donbot.discord_channel_override_explicit);
        assert!(saved.donbot.selected_discord_channel_id.is_empty());
        assert_eq!(h.controller.snapshot().state, State::Verified);
        assert_eq!(
            authorized_donbot_delivery(&saved, &h.controller.snapshot()),
            DonBotDeliveryRoute::None
        );
    }
}

#[test]
fn stale_unmarked_channel_is_cleared_and_defaults_authorized() {
    let mut initial = settings();
    initial.donbot.discord_channel_override_explicit = false;
    let mut h = Harness::new(initial);
    h.verified();
    assert!(h.settings().donbot.selected_discord_channel_id.is_empty());
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::GuildDefaults
    );
}

#[test]
fn refresh_cannot_rebind_old_identity_to_a_changed_endpoint() {
    let mut h = Harness::new(settings());
    h.verified();
    let mut changed = h.settings();
    changed.donbot.api_base_url = "https://other.invalid".into();
    h.configuration.save_settings(changed).unwrap();
    h.storage.lock().unwrap().events.clear();
    assert_eq!(
        h.controller.begin_saved_refresh(&h.configuration),
        Err(Error::StaleVerification)
    );
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::None
    );
    assert!(h.verification.lock().unwrap().requests.is_empty());
    assert!(h.storage.lock().unwrap().events.is_empty());
}

#[test]
fn refresh_retains_controls_blocks_mutations_and_failure_is_nondestructive() {
    let mut h = Harness::new(settings());
    h.verified();
    let before = h.controller.snapshot();
    h.controller.begin_saved_refresh(&h.configuration).unwrap();
    let during = h.controller.snapshot();
    assert_eq!(during.state, State::Verified);
    assert!(during.refreshing);
    assert_eq!(during.guilds, before.guilds);
    assert_eq!(
        h.controller
            .select_guild(&mut h.configuration, "789".into()),
        Err(Error::Busy)
    );
    assert_eq!(
        h.controller
            .select_discord_channel(&mut h.configuration, String::new()),
        Err(Error::Busy)
    );
    assert_eq!(
        h.controller
            .set_discord_delivery_enabled(&mut h.configuration, false),
        Err(Error::Busy)
    );
    h.reply(Err(DonBotVerificationFailure::Failed));
    assert_eq!(h.poll(), Err(Error::VerificationFailed));
    let after = h.controller.snapshot();
    assert!(!after.refreshing);
    assert_eq!(after.state, State::Verified);
    assert_eq!(after.guilds, before.guilds);
    assert_eq!(after.account_name, before.account_name);
    assert_eq!(after.selected_guild_id, before.selected_guild_id);
    assert_eq!(h.settings(), settings());
    assert_eq!(h.storage.lock().unwrap().events, ["load"]);
    h.controller.begin_saved_refresh(&h.configuration).unwrap();
    let mut new_identity = identity();
    new_identity.guilds.clear();
    h.reply(Ok(new_identity));
    assert_eq!(h.poll(), Ok(true));
    assert_disabled(&h.settings());
}

#[test]
fn refresh_load_dispatch_and_save_failures_preserve_last_verified_snapshot() {
    for fail in 0..3 {
        let mut h = Harness::new(settings());
        h.verified();
        let before = h.controller.snapshot();
        match fail {
            0 => h.storage.lock().unwrap().fail_secret = true,
            1 => h.verification.lock().unwrap().reject = true,
            _ => h.storage.lock().unwrap().fail_save = true,
        }
        let outcome = h.controller.begin_saved_refresh(&h.configuration);
        if fail == 2 {
            outcome.unwrap();
            let mut revoked = identity();
            revoked.guilds.clear();
            h.reply(Ok(revoked));
            assert_eq!(h.poll(), Err(Error::SettingsSaveFailed));
        } else {
            assert!(outcome.is_err());
        }
        let after = h.controller.snapshot();
        assert_eq!(after.state, State::Verified);
        assert!(!after.refreshing);
        assert_eq!(after.guilds, before.guilds);
        assert_eq!(h.settings(), settings());
    }
}

#[test]
fn guild_selection_requires_current_membership_and_preserves_route_only_for_same_guild() {
    let mut h = Harness::new(settings());
    assert_eq!(
        h.controller
            .select_guild(&mut h.configuration, "123".into()),
        Err(Error::NotVerified)
    );
    h.verified();
    assert_eq!(
        h.controller
            .select_guild(&mut h.configuration, "999".into()),
        Err(Error::UnknownGuild)
    );
    h.controller
        .select_guild(&mut h.configuration, "123".into())
        .unwrap();
    assert_eq!(h.settings(), settings());
    h.controller
        .select_guild(&mut h.configuration, "789".into())
        .unwrap();
    assert_eq!(h.controller.snapshot().selected_guild_id, "789");
    assert!(!h.settings().donbot.discord_delivery_enabled);
    assert!(!h.settings().donbot.discord_channel_override_explicit);
    assert!(h.settings().donbot.enabled);
    let mut changed = h.settings();
    changed.donbot.api_base_url = "https://other.invalid".into();
    h.configuration.save_settings(changed).unwrap();
    assert_eq!(
        h.controller
            .select_guild(&mut h.configuration, "123".into()),
        Err(Error::StaleVerification)
    );
}

#[test]
fn delivery_toggle_preserves_aggregate_route_and_channel_choice_is_explicit() {
    let mut h = Harness::new(settings());
    h.verified();
    h.controller
        .set_discord_delivery_enabled(&mut h.configuration, false)
        .unwrap();
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::None
    );
    assert_eq!(
        authorized_donbot_aggregate_route(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::ChannelOverride {
            channel_id: "456".into()
        }
    );
    assert_eq!(
        h.controller
            .select_discord_channel(&mut h.configuration, "999".into()),
        Err(Error::UnknownChannel)
    );
    h.controller
        .select_discord_channel(&mut h.configuration, String::new())
        .unwrap();
    assert!(!h.settings().donbot.discord_channel_override_explicit);
    assert!(h.settings().donbot.selected_discord_channel_id.is_empty());
    h.controller
        .set_discord_delivery_enabled(&mut h.configuration, true)
        .unwrap();
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::GuildDefaults
    );
    h.controller
        .select_discord_channel(&mut h.configuration, "456".into())
        .unwrap();
    assert!(h.settings().donbot.discord_channel_override_explicit);
}

#[test]
fn delivery_authorization_checks_live_settings_policy_and_base() {
    let mut h = Harness::new(settings());
    h.verified();
    let snapshot = h.controller.snapshot();
    for mutation in 0..7 {
        let mut settings = h.settings();
        let mut snapshot = snapshot.clone();
        match mutation {
            0 => settings.donbot.enabled = false,
            1 => settings.donbot.selected_guild_id = "789".into(),
            2 => settings.donbot.api_base_url = "https://other.invalid".into(),
            3 => snapshot.discord_summary_delivery_v1 = false,
            4 => snapshot.guilds[0].discord_delivery.pve_summary = false,
            5 => snapshot.guilds[0].discord_delivery.enabled = false,
            6 => settings.donbot.selected_discord_channel_id = "999".into(),
            _ => unreachable!(),
        }
        assert_eq!(
            authorized_donbot_delivery(&settings, &snapshot),
            DonBotDeliveryRoute::None
        );
    }
    let mut no_kinds = identity();
    no_kinds.guilds[0].discord_delivery.pve_summary = false;
    h.controller.begin_saved_refresh(&h.configuration).unwrap();
    h.reply(Ok(no_kinds));
    h.poll().unwrap();
    assert_eq!(
        h.controller
            .set_discord_delivery_enabled(&mut h.configuration, true),
        Err(Error::DeliveryUnavailable)
    );
}

#[test]
fn disconnect_saves_then_erases_and_storage_failure_cannot_leave_uploads_enabled() {
    for fail in 0..3 {
        let mut h = Harness::new(settings());
        h.verified();
        h.storage.lock().unwrap().fail_save = fail == 1;
        h.storage.lock().unwrap().fail_secret = fail == 2;
        let result = h.controller.disconnect(&mut h.configuration);
        if fail == 1 {
            assert_eq!(result, Err(Error::SettingsSaveFailed));
            assert_eq!(h.storage.lock().unwrap().events, ["save"]);
            assert_eq!(h.settings(), settings());
        } else {
            assert_eq!(h.storage.lock().unwrap().events, ["save", "erase"]);
            assert_disabled(&h.settings());
            assert!(h.controller.snapshot().guilds.is_empty());
            assert_eq!(
                result,
                if fail == 2 {
                    Err(Error::SecretEraseFailed)
                } else {
                    Ok(())
                }
            );
        }
    }
}

#[test]
fn invalid_input_missing_storage_and_dispatch_rejection_do_not_persist() {
    for base in [
        "http://donbot.invalid",
        "https://donbot.invalid?secret=hidden",
        "https://donbot.invalid/../private",
    ] {
        let mut h = Harness::new(settings());
        assert_eq!(
            h.controller
                .begin_verification(&h.configuration, base.into(), key()),
            Err(Error::InvalidConfiguration)
        );
        assert!(h.verification.lock().unwrap().requests.is_empty());
        assert!(h.storage.lock().unwrap().events.is_empty());
        assert!(!format!("{:?}", h.controller.snapshot()).contains("hidden"));
    }
    let mut h = Harness::new(settings());
    h.verification.lock().unwrap().reject = true;
    assert_eq!(
        h.controller
            .begin_verification(&h.configuration, BASE.into(), key()),
        Err(Error::DispatchFailed)
    );
    assert!(h.storage.lock().unwrap().events.is_empty());
    let configuration = ConfigurationService::new(
        Box::new(Ordinary(h.storage.clone(), settings())),
        Err(secret_error()),
    )
    .unwrap();
    assert_eq!(
        h.controller.begin_saved_verification(&configuration),
        Err(Error::SecretLoadFailed)
    );
    assert!(h.verification.lock().unwrap().requests.is_empty());
}

#[test]
fn adapter_panics_are_contained_and_shutdown_cancels_once() {
    for panic in 0..3 {
        let mut h = Harness::new(settings());
        if panic == 0 {
            h.verification.lock().unwrap().panic_enqueue = true;
            assert_eq!(
                h.controller
                    .begin_verification(&h.configuration, BASE.into(), key()),
                Err(Error::DispatchFailed)
            );
        } else {
            h.begin();
            h.reply(Ok(identity()));
            if panic == 1 {
                h.verification.lock().unwrap().panic_poll = true;
            } else {
                h.storage.lock().unwrap().panic_save = true;
            }
            assert!(h.poll().is_err());
        }
        assert_eq!(h.controller.snapshot().state, State::Error);
    }
    let mut h = Harness::new(settings());
    h.begin();
    h.controller.shutdown();
    let snapshot = h.controller.snapshot();
    h.controller.shutdown();
    assert_eq!(h.controller.snapshot(), snapshot);
    assert_eq!(snapshot.state, State::ShuttingDown);
    assert!(snapshot.guilds.is_empty());
    assert_eq!(h.poll(), Err(Error::ShuttingDown));
    assert_eq!(
        h.controller.disconnect(&mut h.configuration),
        Err(Error::ShuttingDown)
    );
    let verification = h.verification.clone();
    drop(h);
    assert_eq!(verification.lock().unwrap().cancellations, 1);
}

#[test]
fn new_candidate_base_is_persisted_but_never_implicitly_enables_uploads() {
    let mut h = Harness::new(settings());
    h.controller
        .begin_verification(&h.configuration, "https://new.invalid/api/".into(), key())
        .unwrap();
    h.reply(Ok(identity()));
    h.poll().unwrap();
    assert_eq!(h.settings().donbot.api_base_url, "https://new.invalid/api");
    h.controller
        .select_guild(&mut h.configuration, "123".into())
        .unwrap();
    assert_eq!(h.settings().donbot.selected_guild_id, "123");
    assert!(!h.settings().donbot.enabled);
    assert_eq!(
        h.controller
            .set_discord_delivery_enabled(&mut h.configuration, true),
        Err(Error::DeliveryUnavailable)
    );
}

#[test]
fn absent_default_delivery_and_aggregate_defaults_are_checked_separately() {
    let mut initial = settings();
    initial.donbot.discord_channel_override_explicit = false;
    initial.donbot.selected_discord_channel_id.clear();
    let mut h = Harness::new(initial);
    h.controller
        .begin_saved_verification(&h.configuration)
        .unwrap();
    let mut current = identity();
    current.guilds[0]
        .discord_delivery
        .aggregate_defaults_available = false;
    h.reply(Ok(current));
    h.poll().unwrap();
    assert_eq!(
        authorized_donbot_delivery(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::GuildDefaults
    );
    assert_eq!(
        authorized_donbot_aggregate_route(&h.settings(), &h.controller.snapshot()),
        DonBotDeliveryRoute::None
    );
    h.controller.begin_saved_refresh(&h.configuration).unwrap();
    let mut current = identity();
    current.guilds[0].discord_delivery.defaults_available = false;
    h.reply(Ok(current));
    h.poll().unwrap();
    assert!(!h.settings().donbot.discord_delivery_enabled);
    assert_eq!(
        h.controller
            .select_discord_channel(&mut h.configuration, String::new()),
        Err(Error::DeliveryUnavailable)
    );
    h.controller
        .select_discord_channel(&mut h.configuration, "456".into())
        .unwrap();
    h.controller
        .set_discord_delivery_enabled(&mut h.configuration, true)
        .unwrap();
}

#[test]
fn failed_guild_save_never_changes_durable_selection_and_shutdown_blocks_ports() {
    let mut h = Harness::new(settings());
    h.verified();
    h.storage.lock().unwrap().fail_save = true;
    assert_eq!(
        h.controller
            .select_guild(&mut h.configuration, "789".into()),
        Err(Error::SettingsSaveFailed)
    );
    assert_eq!(h.settings(), settings());
    assert_ne!(h.controller.snapshot().selected_guild_id, "789");
    h.storage.lock().unwrap().events.clear();
    h.configuration.shutdown();
    assert_eq!(
        h.controller.begin_saved_verification(&h.configuration),
        Err(Error::ShuttingDown)
    );
    assert_eq!(
        h.controller.disconnect(&mut h.configuration),
        Err(Error::ShuttingDown)
    );
    assert_eq!(h.poll(), Err(Error::ShuttingDown));
    assert!(h.storage.lock().unwrap().events.is_empty());
}
