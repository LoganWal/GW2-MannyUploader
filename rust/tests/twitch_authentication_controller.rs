use manny_uploader::{
    application::*,
    domain::{Settings, TwitchDeliveryReceipt},
};
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
    time::{Duration, Instant, SystemTime},
};
type Error = TwitchAuthenticationControllerError;
type State = TwitchConnectionState;

fn secret(value: &[u8]) -> SecretValue {
    SecretValue::from_bytes(value.to_vec())
}
fn scopes() -> Vec<String> {
    vec![TWITCH_CHAT_SCOPE.into()]
}
fn identity(seconds: u64) -> TwitchValidatedIdentity {
    TwitchValidatedIdentity {
        user_id: "123456789".into(),
        login: "broadcaster".into(),
        scopes: scopes(),
        expires_in: Duration::from_secs(seconds),
    }
}
fn grant() -> TwitchTokenGrant {
    TwitchTokenGrant {
        access_token: secret(b"PRIVATE-ACCESS"),
        refresh_token: secret(b"PRIVATE-REFRESH"),
        expires_in: Duration::from_secs(14400),
        scopes: scopes(),
    }
}
fn device() -> TwitchDeviceAuthorization {
    TwitchDeviceAuthorization {
        device_code: secret(b"PRIVATE-DEVICE"),
        user_code: "CODE".into(),
        verification_uri: "https://www.twitch.tv/activate".into(),
        expires_in: Duration::from_secs(600),
        polling_interval: Duration::from_secs(5),
    }
}
fn saved_session() -> TwitchSession {
    TwitchSession {
        credentials: TwitchCredentialSet {
            access_token: secret(b"SAVED-ACCESS"),
            refresh_token: secret(b"SAVED-REFRESH"),
            access_expires_at: SystemTime::UNIX_EPOCH + Duration::from_secs(18000),
        },
        user_id: "123456789".into(),
        login: "saved_name".into(),
        scopes: scopes(),
    }
}
#[derive(Default)]
struct Storage {
    bytes: Option<Vec<u8>>,
    events: Vec<String>,
    fail_save: bool,
    fail_store: bool,
    fail_erase: bool,
    panic_save: bool,
}
struct Ordinary(Arc<Mutex<Storage>>);
impl SettingsStore for Ordinary {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        let mut settings = Settings::new("synthetic-logs".into());
        settings.twitch.enabled = true;
        Ok(SettingsLoadResult {
            settings,
            source: SettingsLoadSource::Primary,
            recovery: None,
        })
    }
    fn save(&self, settings: &Settings) -> Result<(), SettingsStoreError> {
        assert!(!settings.twitch.enabled);
        let mut storage = self.0.lock().unwrap();
        storage.events.push("settings.save".into());
        if storage.panic_save {
            drop(storage);
            panic!("synthetic settings panic");
        }
        if storage.fail_save {
            return Err(SettingsStoreError::InvalidValues(vec![]));
        }
        Ok(())
    }
}
fn secret_error() -> SecretStoreError {
    SecretStoreError {
        id: Some(SecretId::TwitchOAuthSession),
        code: SecretStoreErrorCode::NotFound,
        system_error: None,
    }
}
struct Protected(Arc<Mutex<Storage>>);
impl SecretStore for Protected {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::NativeUserScoped
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        assert_eq!(id, SecretId::TwitchOAuthSession);
        self.0
            .lock()
            .unwrap()
            .bytes
            .as_ref()
            .map(|bytes| secret(bytes))
            .ok_or_else(secret_error)
    }
    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::TwitchOAuthSession);
        decode_twitch_session(value).unwrap();
        let mut storage = self.0.lock().unwrap();
        storage.events.push("secret.store".into());
        if storage.fail_store {
            return Err(secret_error());
        }
        storage.bytes = Some(value.expose().to_vec());
        Ok(())
    }
    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::TwitchOAuthSession);
        let mut storage = self.0.lock().unwrap();
        storage.events.push("secret.erase".into());
        if storage.fail_erase {
            return Err(secret_error());
        }
        storage.bytes = None;
        Ok(())
    }
}
struct TestClock(Mutex<(Instant, SystemTime)>);
impl TestClock {
    fn new() -> Self {
        Self(Mutex::new((
            Instant::now(),
            SystemTime::UNIX_EPOCH + Duration::from_secs(10000),
        )))
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
struct NoHttp;
impl TwitchApi for NoHttp {
    fn start_device_authorization(
        &self,
        _: &StopToken,
    ) -> Result<TwitchDeviceAuthorization, TwitchError> {
        panic!("controller must use worker")
    }
    fn poll_device_authorization(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchDevicePollResult, TwitchError> {
        panic!("controller must use worker")
    }
    fn validate_access_token(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchValidatedIdentity, TwitchError> {
        panic!("controller must use worker")
    }
    fn refresh_access_token(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchTokenGrant, TwitchError> {
        panic!("controller must use worker")
    }
    fn revoke_access_token(&self, _: &SecretValue, _: &StopToken) -> Result<(), TwitchError> {
        panic!("controller must use worker")
    }
    fn send_chat_message(
        &self,
        _: &str,
        _: &str,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchDeliveryReceipt, TwitchError> {
        panic!("controller must use worker")
    }
}
#[derive(Default)]
struct WorkerState {
    requests: VecDeque<TwitchAuthenticationRequest>,
    results: VecDeque<TwitchAuthenticationResult>,
    reject: bool,
    panic_enqueue: bool,
    panic_poll: bool,
    cancellations: usize,
}
struct Worker(Arc<Mutex<WorkerState>>, Arc<Mutex<Storage>>);
impl TwitchAuthenticator for Worker {
    fn enqueue(
        &mut self,
        request: TwitchAuthenticationRequest,
    ) -> Result<(), TwitchAuthenticationDispatchError> {
        self.1
            .lock()
            .unwrap()
            .events
            .push(format!("enqueue.{:?}", request.command.operation()));
        let mut state = self.0.lock().unwrap();
        if state.panic_enqueue {
            drop(state);
            panic!("synthetic dispatch panic");
        }
        if state.reject {
            return Err(TwitchAuthenticationDispatchError::QueueFull);
        }
        state.requests.push_back(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<TwitchAuthenticationResult> {
        let mut state = self.0.lock().unwrap();
        if state.panic_poll {
            drop(state);
            panic!("synthetic poll panic");
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
    config: Arc<Mutex<ConfigurationService>>,
    worker: Arc<Mutex<WorkerState>>,
    clock: Arc<TestClock>,
    owner: Arc<TwitchSessionOwner>,
    controller: TwitchAuthenticationController,
}
impl Harness {
    fn new(saved: bool) -> Self {
        let storage = Arc::new(Mutex::new(Storage::default()));
        if saved {
            storage.lock().unwrap().bytes = Some(
                encode_twitch_session(&saved_session())
                    .unwrap()
                    .expose()
                    .to_vec(),
            );
        }
        let config = Arc::new(Mutex::new(
            ConfigurationService::new(
                Box::new(Ordinary(storage.clone())),
                Ok(Box::new(Protected(storage.clone()))),
            )
            .unwrap(),
        ));
        let worker = Arc::new(Mutex::new(WorkerState::default()));
        let clock = Arc::new(TestClock::new());
        let owner = Arc::new(TwitchSessionOwner::new(
            config.clone(),
            Arc::new(NoHttp),
            clock.clone(),
        ));
        let controller = TwitchAuthenticationController::new(
            config.clone(),
            Box::new(Worker(worker.clone(), storage.clone())),
            owner.clone(),
            clock.clone(),
        )
        .unwrap();
        Self {
            storage,
            config,
            worker,
            clock,
            owner,
            controller,
        }
    }
    fn reply(&self, f: impl FnOnce(TwitchAuthenticationCommand) -> TwitchAuthenticationSuccess) {
        let mut worker = self.worker.lock().unwrap();
        let request = worker.requests.pop_front().unwrap();
        let operation = request.command.operation();
        worker.results.push_back(TwitchAuthenticationResult {
            request_id: request.request_id,
            operation,
            outcome: Ok(f(request.command)),
            retained: None,
        });
    }
    fn fail(&self, disposition: TwitchDisposition, seconds: u64) {
        let mut worker = self.worker.lock().unwrap();
        let request = worker.requests.pop_front().unwrap();
        let operation = request.command.operation();
        let retained = match request.command {
            TwitchAuthenticationCommand::Poll { device_code } => {
                Some(TwitchAuthenticationSecret::DeviceCode(device_code))
            }
            TwitchAuthenticationCommand::Validate { credentials }
            | TwitchAuthenticationCommand::Refresh { credentials } => {
                Some(TwitchAuthenticationSecret::Credentials(credentials))
            }
            _ => None,
        };
        worker.results.push_back(TwitchAuthenticationResult {
            request_id: request.request_id,
            operation,
            outcome: Err(TwitchError {
                disposition,
                retry_after: Some(Duration::from_secs(seconds)),
                ..TwitchError::failed(operation, TwitchErrorCode::Transport)
            }),
            retained,
        });
    }
    fn validate(&self, identity: TwitchValidatedIdentity) {
        self.reply(|command| {
            let TwitchAuthenticationCommand::Validate { credentials } = command else {
                panic!("expected validate")
            };
            TwitchAuthenticationSuccess::Validated {
                credentials,
                identity,
            }
        });
    }
    fn tick(&mut self) {
        assert_eq!(self.controller.tick(), Ok(true));
    }
    fn awaiting(&mut self) {
        self.controller.begin_connection().unwrap();
        self.reply(|command| {
            assert!(matches!(command, TwitchAuthenticationCommand::Start));
            TwitchAuthenticationSuccess::Started(device())
        });
        self.tick();
    }
    fn granted(&mut self) {
        self.awaiting();
        self.clock.advance(5);
        self.tick();
        self.reply(|command| {
            assert!(matches!(command, TwitchAuthenticationCommand::Poll { .. }));
            TwitchAuthenticationSuccess::Granted(grant())
        });
        self.tick();
    }
    fn connected(&mut self, seconds: u64) {
        self.granted();
        self.validate(identity(seconds));
        self.tick();
        self.storage.lock().unwrap().events.clear();
    }
    fn operation(&self) -> TwitchOperation {
        self.worker
            .lock()
            .unwrap()
            .requests
            .front()
            .unwrap()
            .command
            .operation()
    }
    fn events(&self) -> Vec<String> {
        self.storage.lock().unwrap().events.clone()
    }
    fn empty(&self) {
        assert!(self.worker.lock().unwrap().requests.is_empty());
    }
    fn unavailable(&self, code: TwitchDeliverySessionErrorCode) {
        assert_eq!(
            self.owner
                .acquire(&CancellationSource::default().token())
                .unwrap_err()
                .code,
            code
        );
    }
    fn saved(&self) -> TwitchSession {
        decode_twitch_session(&secret(
            self.storage.lock().unwrap().bytes.as_ref().unwrap(),
        ))
        .unwrap()
    }
}

#[test]
fn device_flow_waits_for_interval_and_validation_before_storage_and_publication() {
    let mut h = Harness::new(false);
    h.awaiting();
    assert_eq!(h.controller.snapshot().state, State::AwaitingUser);
    assert_eq!(h.controller.snapshot().user_code.as_deref(), Some("CODE"));
    h.unavailable(TwitchDeliverySessionErrorCode::Retry);
    assert!(h.storage.lock().unwrap().bytes.is_none());
    assert_eq!(h.controller.tick(), Ok(false));
    h.clock.advance(4);
    assert_eq!(h.controller.tick(), Ok(false));
    h.empty();
    h.clock.advance(1);
    h.tick();
    h.reply(|command| {
        let TwitchAuthenticationCommand::Poll { device_code } = command else {
            panic!()
        };
        TwitchAuthenticationSuccess::Pending { device_code }
    });
    h.tick();
    h.clock.advance(4);
    assert_eq!(h.controller.tick(), Ok(false));
    h.clock.advance(1);
    h.tick();
    h.reply(|_| TwitchAuthenticationSuccess::Granted(grant()));
    h.tick();
    assert!(h.controller.snapshot().user_code.is_none());
    assert!(h.storage.lock().unwrap().bytes.is_none());
    h.validate(identity(14400));
    h.tick();
    assert_eq!(h.controller.snapshot().state, State::Connected);
    assert_eq!(
        h.controller.snapshot().login.as_deref(),
        Some("broadcaster")
    );
    assert_eq!(
        h.saved().credentials.refresh_token.expose(),
        b"PRIVATE-REFRESH"
    );
    let printed = format!("{:?}", h.controller.snapshot());
    for private in [
        "123456789",
        "PRIVATE-ACCESS",
        "PRIVATE-REFRESH",
        "PRIVATE-DEVICE",
    ] {
        assert!(!printed.contains(private));
    }
}

#[test]
fn startup_loads_without_publishing_stored_identity() {
    let mut missing = Harness::new(false);
    assert_eq!(missing.controller.begin_saved_connection(), Ok(false));
    missing.empty();
    let mut corrupt = Harness::new(false);
    corrupt.storage.lock().unwrap().bytes = Some(vec![1]);
    assert_eq!(
        corrupt.controller.begin_saved_connection(),
        Err(Error::SessionDecodeFailed)
    );
    corrupt.empty();
    let mut h = Harness::new(true);
    assert_eq!(h.controller.begin_saved_connection(), Ok(true));
    assert!(h.controller.snapshot().login.is_none());
    h.unavailable(TwitchDeliverySessionErrorCode::Retry);
    let mut renamed = identity(14400);
    renamed.login = "renamed".into();
    h.validate(renamed);
    h.tick();
    assert_eq!(h.saved().login, "renamed");
    assert_eq!(h.events(), ["enqueue.Validate", "secret.store"]);
}

#[test]
fn startup_refresh_stores_both_tokens_before_validation_and_rejects_a_second_reconnect() {
    for second_reconnect in [false, true] {
        let mut h = Harness::new(true);
        h.controller.begin_saved_connection().unwrap();
        h.fail(TwitchDisposition::Reconnect, 0);
        h.tick();
        assert_eq!(h.operation(), TwitchOperation::Refresh);
        h.reply(|_| TwitchAuthenticationSuccess::Refreshed(grant()));
        h.tick();
        assert_eq!(
            h.saved().credentials.access_token.expose(),
            b"PRIVATE-ACCESS"
        );
        assert_eq!(
            h.saved().credentials.refresh_token.expose(),
            b"PRIVATE-REFRESH"
        );
        assert_eq!(h.saved().login, "saved_name");
        assert_eq!(
            h.events(),
            [
                "enqueue.Validate",
                "enqueue.Refresh",
                "secret.store",
                "enqueue.Validate"
            ]
        );
        h.unavailable(TwitchDeliverySessionErrorCode::Retry);
        if second_reconnect {
            h.fail(TwitchDisposition::Reconnect, 0);
            assert_eq!(h.controller.tick(), Err(Error::ReconnectRequired));
            assert!(h.storage.lock().unwrap().bytes.is_none());
            h.empty();
        } else {
            h.validate(identity(14400));
            h.tick();
            assert_eq!(h.controller.snapshot().state, State::Connected);
        }
    }
}

#[test]
fn validation_and_refresh_retries_keep_the_transaction_and_respect_monotonic_deadlines() {
    for refresh in [false, true] {
        let mut h = Harness::new(true);
        h.controller.begin_saved_connection().unwrap();
        if refresh {
            h.fail(TwitchDisposition::Reconnect, 0);
            h.tick();
        }
        h.fail(TwitchDisposition::Retry, 17);
        h.tick();
        h.empty();
        h.unavailable(TwitchDeliverySessionErrorCode::Retry);
        h.clock.advance(16);
        assert_eq!(h.controller.tick(), Ok(false));
        h.empty();
        h.clock.0.lock().unwrap().1 += Duration::from_secs(100000);
        assert_eq!(h.controller.tick(), Ok(false));
        h.clock.advance(1);
        h.tick();
        assert_eq!(
            h.operation(),
            if refresh {
                TwitchOperation::Refresh
            } else {
                TwitchOperation::Validate
            }
        );
    }
}

#[test]
fn poll_retry_never_violates_the_server_interval_and_bounds_invalid_delays() {
    for (delay, expected) in [(1, 5), (17, 17), (0, 30), (86401, 30)] {
        let mut h = Harness::new(false);
        h.awaiting();
        h.clock.advance(5);
        h.tick();
        h.fail(TwitchDisposition::Retry, delay);
        h.tick();
        h.clock.advance(expected - 1);
        assert_eq!(h.controller.tick(), Ok(false));
        h.clock.advance(1);
        h.tick();
        assert_eq!(h.operation(), TwitchOperation::Poll);
    }
}

#[test]
fn hourly_validation_and_five_minute_refresh_ignore_wall_clock_jumps() {
    for (lifetime, deadline, operation) in [
        (14400, 3600, TwitchOperation::Validate),
        (3600, 3300, TwitchOperation::Refresh),
    ] {
        let mut h = Harness::new(false);
        h.connected(lifetime);
        h.clock.0.lock().unwrap().1 += Duration::from_secs(100000);
        h.clock.advance(deadline - 1);
        assert_eq!(h.controller.tick(), Ok(false));
        h.empty();
        h.clock.advance(1);
        h.tick();
        assert_eq!(h.operation(), operation);
        h.unavailable(TwitchDeliverySessionErrorCode::Retry);
    }
}

#[test]
fn restored_chat_transaction_does_not_postpone_hourly_validation() {
    let mut h = Harness::new(false);
    h.connected(14400);
    h.clock.advance(3599);
    let tx = h.owner.checkout_active().unwrap();
    h.tick();
    assert_eq!(h.controller.snapshot().state, State::Validating);
    h.owner.restore(tx.id, tx.session).unwrap();
    h.tick();
    assert_eq!(h.controller.snapshot().state, State::Connected);
    h.clock.advance(1);
    h.tick();
    assert_eq!(h.operation(), TwitchOperation::Validate);
}

#[test]
fn new_chat_generation_updates_public_identity_and_expiry() {
    let mut h = Harness::new(false);
    h.connected(14400);
    let mut tx = h.owner.checkout_active().unwrap();
    h.tick();
    tx.session.login = "renamed".into();
    h.owner.commit(tx.id, tx.session).unwrap();
    h.tick();
    assert_eq!(h.controller.snapshot().login.as_deref(), Some("renamed"));
    h.empty();
    let tx = h.owner.checkout_active().unwrap();
    h.owner.discard(tx.id, true).unwrap();
    assert_eq!(h.controller.tick(), Err(Error::ReconnectRequired));
    assert!(h.controller.snapshot().login.is_none());
}

#[test]
fn failed_initial_persistence_never_connects() {
    let mut h = Harness::new(false);
    h.granted();
    h.storage.lock().unwrap().fail_store = true;
    h.validate(identity(14400));
    assert_eq!(h.controller.tick(), Err(Error::SessionStoreFailed));
    assert_eq!(h.controller.snapshot().state, State::Error);
    assert!(h.controller.snapshot().login.is_none());
    h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
}

#[test]
fn rotation_persistence_failure_erases_the_old_record_and_stops_validation() {
    let mut h = Harness::new(true);
    h.controller.begin_saved_connection().unwrap();
    h.fail(TwitchDisposition::Reconnect, 0);
    h.tick();
    h.storage.lock().unwrap().fail_store = true;
    h.reply(|_| TwitchAuthenticationSuccess::Refreshed(grant()));
    assert_eq!(h.controller.tick(), Err(Error::SessionStoreFailed));
    assert_eq!(
        h.events(),
        [
            "enqueue.Validate",
            "enqueue.Refresh",
            "secret.store",
            "secret.erase"
        ]
    );
    assert!(h.storage.lock().unwrap().bytes.is_none());
    h.empty();
    h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
}

#[test]
fn identity_change_erases_the_session() {
    let mut h = Harness::new(true);
    h.controller.begin_saved_connection().unwrap();
    let mut other = identity(14400);
    other.user_id = "999".into();
    h.validate(other);
    assert_eq!(h.controller.tick(), Err(Error::IdentityMismatch));
    assert!(h.storage.lock().unwrap().bytes.is_none());
    h.empty();
}

#[test]
fn disconnect_disables_before_revocation_and_erases_even_after_remote_failure() {
    for fails in [false, true] {
        let mut h = Harness::new(false);
        h.connected(14400);
        h.controller.disconnect().unwrap();
        assert_eq!(h.events(), ["settings.save", "enqueue.Revoke"]);
        assert!(!h.config.lock().unwrap().snapshot().settings.twitch.enabled);
        assert_eq!(h.controller.snapshot().state, State::Disconnecting);
        h.unavailable(TwitchDeliverySessionErrorCode::Retry);
        assert!(h.storage.lock().unwrap().bytes.is_some());
        if fails {
            h.fail(TwitchDisposition::Retry, 60);
            assert_eq!(h.controller.tick(), Err(Error::AuthenticationFailed));
        } else {
            h.reply(|_| TwitchAuthenticationSuccess::Revoked);
            h.tick();
        }
        assert_eq!(h.controller.snapshot().state, State::Disconnected);
        assert!(h.storage.lock().unwrap().bytes.is_none());
        h.empty();
    }
}

#[test]
fn failed_settings_save_preserves_session_and_does_not_revoke() {
    for panic in [false, true] {
        let mut h = Harness::new(false);
        h.connected(14400);
        {
            let mut store = h.storage.lock().unwrap();
            store.fail_save = true;
            store.panic_save = panic;
        }
        assert_eq!(h.controller.disconnect(), Err(Error::SettingsSaveFailed));
        assert_eq!(h.events(), ["settings.save"]);
        assert!(h.storage.lock().unwrap().bytes.is_some());
        assert_eq!(h.controller.snapshot().state, State::Connected);
        assert!(h.config.lock().unwrap().snapshot().settings.twitch.enabled);
        h.empty();
    }
}

#[test]
fn disconnect_dispatch_or_erase_failure_leaves_posting_disabled() {
    for erase_failure in [false, true] {
        let mut h = Harness::new(false);
        h.connected(14400);
        h.worker.lock().unwrap().reject = true;
        h.storage.lock().unwrap().fail_erase = erase_failure;
        assert_eq!(
            h.controller.disconnect(),
            Err(if erase_failure {
                Error::SessionEraseFailed
            } else {
                Error::DispatchFailed
            })
        );
        assert!(!h.config.lock().unwrap().snapshot().settings.twitch.enabled);
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
        assert_eq!(h.storage.lock().unwrap().bytes.is_some(), erase_failure);
    }
}

#[test]
fn authorization_can_be_cancelled_between_polls() {
    let mut h = Harness::new(false);
    h.awaiting();
    h.storage.lock().unwrap().events.clear();
    h.controller.disconnect().unwrap();
    assert_eq!(h.controller.snapshot().state, State::Disconnected);
    assert!(h.controller.snapshot().user_code.is_none());
    assert_eq!(h.events(), ["settings.save", "secret.erase"]);
    h.empty();
}

#[test]
fn expiry_rejects_an_in_flight_late_grant() {
    let mut h = Harness::new(false);
    h.awaiting();
    h.clock.advance(5);
    h.tick();
    h.reply(|_| TwitchAuthenticationSuccess::Granted(grant()));
    h.clock.advance(595);
    assert_eq!(h.controller.tick(), Err(Error::AuthorizationExpired));
    assert!(h.controller.snapshot().verification_uri.is_none());
    assert!(h.storage.lock().unwrap().bytes.is_none());
    h.empty();
    assert_eq!(h.controller.tick(), Err(Error::StaleResult));
    h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
}

#[test]
fn stale_result_does_not_cancel_the_current_request() {
    let mut h = Harness::new(false);
    h.controller.begin_connection().unwrap();
    h.worker
        .lock()
        .unwrap()
        .results
        .push_back(TwitchAuthenticationResult {
            request_id: 999,
            operation: TwitchOperation::Start,
            outcome: Ok(TwitchAuthenticationSuccess::Started(device())),
            retained: None,
        });
    assert_eq!(h.controller.tick(), Err(Error::StaleResult));
    assert_eq!(h.controller.snapshot().state, State::Starting);
    h.reply(|_| TwitchAuthenticationSuccess::Started(device()));
    h.tick();
    assert_eq!(h.controller.snapshot().state, State::AwaitingUser);
}

#[test]
fn malformed_correlated_results_fail_without_publication() {
    for wrong_operation in [false, true] {
        let mut h = Harness::new(false);
        h.controller.begin_connection().unwrap();
        h.reply(|_| TwitchAuthenticationSuccess::Revoked);
        if wrong_operation {
            h.worker
                .lock()
                .unwrap()
                .results
                .front_mut()
                .unwrap()
                .operation = TwitchOperation::Validate;
        }
        assert_eq!(h.controller.tick(), Err(Error::InvalidResult));
        assert_eq!(h.controller.snapshot().state, State::Error);
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
    }
}

#[test]
fn worker_panics_are_contained_and_release_the_owner() {
    for enqueue in [false, true] {
        let mut h = Harness::new(false);
        if enqueue {
            h.worker.lock().unwrap().panic_enqueue = true;
            assert_eq!(h.controller.begin_connection(), Err(Error::DispatchFailed));
        } else {
            h.controller.begin_connection().unwrap();
            h.worker.lock().unwrap().panic_poll = true;
            assert_eq!(h.controller.tick(), Err(Error::Internal));
        }
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
        assert!(h.controller.snapshot().user_code.is_none());
    }
}

#[test]
fn shutdown_clears_transient_state_cancels_once_and_rejects_commands() {
    let mut h = Harness::new(false);
    h.granted();
    h.controller.shutdown();
    h.controller.shutdown();
    assert_eq!(h.controller.snapshot().state, State::ShuttingDown);
    assert!(h.controller.is_shutting_down());
    assert_eq!(h.worker.lock().unwrap().cancellations, 1);
    h.empty();
    assert_eq!(h.controller.begin_connection(), Err(Error::ShuttingDown));
    assert_eq!(h.controller.tick(), Err(Error::ShuttingDown));
    assert_eq!(h.controller.disconnect(), Err(Error::ShuttingDown));
    h.unavailable(TwitchDeliverySessionErrorCode::Cancelled);
    assert!(h.storage.lock().unwrap().bytes.is_none());
}

#[test]
fn disconnect_erases_after_poll_panic_or_malformed_retained_success() {
    for panic in [false, true] {
        let mut h = Harness::new(false);
        h.connected(14400);
        h.controller.disconnect().unwrap();
        if panic {
            h.worker.lock().unwrap().panic_poll = true;
        } else {
            h.reply(|_| TwitchAuthenticationSuccess::Revoked);
            h.worker
                .lock()
                .unwrap()
                .results
                .front_mut()
                .unwrap()
                .retained = Some(TwitchAuthenticationSecret::DeviceCode(secret(
                b"UNEXPECTED",
            )));
        }
        assert!(h.controller.tick().is_err());
        assert!(h.storage.lock().unwrap().bytes.is_none());
        assert_eq!(h.controller.snapshot().state, State::Disconnected);
    }
}

#[test]
fn shutdown_during_explicit_disconnect_erases_before_invalidating_owner() {
    let mut h = Harness::new(false);
    h.connected(14400);
    h.controller.disconnect().unwrap();
    h.controller.shutdown();
    assert!(h.storage.lock().unwrap().bytes.is_none());
    assert!(!h.config.lock().unwrap().snapshot().settings.twitch.enabled);
    assert_eq!(h.worker.lock().unwrap().cancellations, 1);
}

#[test]
fn initial_reconnect_and_missing_retry_credentials_do_not_start_refresh() {
    for missing_credentials in [false, true] {
        let mut h = Harness::new(false);
        h.granted();
        h.fail(
            if missing_credentials {
                TwitchDisposition::Retry
            } else {
                TwitchDisposition::Reconnect
            },
            30,
        );
        if missing_credentials {
            h.worker
                .lock()
                .unwrap()
                .results
                .front_mut()
                .unwrap()
                .retained = None;
        }
        assert_eq!(
            h.controller.tick(),
            Err(if missing_credentials {
                Error::InvalidResult
            } else {
                Error::ReconnectRequired
            })
        );
        h.empty();
        assert!(h.storage.lock().unwrap().bytes.is_none());
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
    }
}

#[test]
fn replacement_validation_retry_uses_the_rotated_pair_without_a_second_refresh() {
    let mut h = Harness::new(true);
    h.controller.begin_saved_connection().unwrap();
    h.fail(TwitchDisposition::Reconnect, 0);
    h.tick();
    h.reply(|_| TwitchAuthenticationSuccess::Refreshed(grant()));
    h.tick();
    h.fail(TwitchDisposition::Retry, 17);
    h.tick();
    h.empty();
    h.unavailable(TwitchDeliverySessionErrorCode::Retry);
    assert_eq!(
        h.saved().credentials.refresh_token.expose(),
        b"PRIVATE-REFRESH"
    );
    h.clock.advance(17);
    h.tick();
    h.reply(|command| {
        let TwitchAuthenticationCommand::Validate { credentials } = command else {
            panic!("expected validation")
        };
        assert_eq!(credentials.access_token.expose(), b"PRIVATE-ACCESS");
        assert_eq!(credentials.refresh_token.expose(), b"PRIVATE-REFRESH");
        TwitchAuthenticationSuccess::Validated {
            credentials,
            identity: identity(14400),
        }
    });
    h.tick();
    assert_eq!(
        h.events()
            .iter()
            .filter(|event| *event == "enqueue.Refresh")
            .count(),
        1
    );
    assert_eq!(h.controller.snapshot().state, State::Connected);
}

#[test]
fn refresh_terminal_failures_and_invalid_rotations_erase_saved_credentials() {
    for case in 0..4 {
        let mut h = Harness::new(true);
        h.controller.begin_saved_connection().unwrap();
        h.fail(TwitchDisposition::Reconnect, 0);
        h.tick();
        if case == 0 {
            h.fail(TwitchDisposition::Failed, 0);
        } else if case == 1 {
            h.fail(TwitchDisposition::Cancelled, 0);
        } else {
            let mut invalid = grant();
            if case == 2 {
                invalid.scopes.clear();
            } else {
                invalid.expires_in = Duration::ZERO;
            }
            h.reply(|_| TwitchAuthenticationSuccess::Refreshed(invalid));
        }
        assert!(h.controller.tick().is_err());
        assert!(h.storage.lock().unwrap().bytes.is_none());
        h.empty();
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
    }
}

#[test]
fn replacement_dispatch_failure_does_not_leave_rotated_credentials_active() {
    let mut h = Harness::new(true);
    h.controller.begin_saved_connection().unwrap();
    h.fail(TwitchDisposition::Reconnect, 0);
    h.tick();
    h.reply(|_| TwitchAuthenticationSuccess::Refreshed(grant()));
    h.worker.lock().unwrap().reject = true;
    assert_eq!(h.controller.tick(), Err(Error::DispatchFailed));
    assert_eq!(
        h.events(),
        [
            "enqueue.Validate",
            "enqueue.Refresh",
            "secret.store",
            "enqueue.Validate",
            "secret.erase"
        ]
    );
    h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
}

#[test]
fn malformed_device_results_are_rejected_before_entering_public_snapshots() {
    for case in 0..8 {
        let mut h = Harness::new(false);
        h.controller.begin_connection().unwrap();
        let mut invalid = device();
        match case {
            0 => invalid.verification_uri = "https://www.twitch.tv/activate.evil.invalid".into(),
            1 => {
                invalid.verification_uri = "https://www.twitch.tv/activate?token=x#fragment".into()
            }
            2 => invalid.user_code = "CODE\nINJECTED".into(),
            3 => invalid.device_code = secret(b""),
            4 => invalid.polling_interval = Duration::ZERO,
            5 => invalid.polling_interval = Duration::from_secs(301),
            6 => invalid.expires_in = Duration::ZERO,
            _ => invalid.expires_in = Duration::from_secs(604801),
        }
        h.reply(|_| TwitchAuthenticationSuccess::Started(invalid));
        assert_eq!(h.controller.tick(), Err(Error::InvalidResult));
        assert!(h.controller.snapshot().verification_uri.is_none());
        assert!(h.controller.snapshot().user_code.is_none());
        h.empty();
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
    }
}

#[test]
fn invalid_validation_metadata_never_becomes_connected() {
    for case in 0..5 {
        let mut h = Harness::new(false);
        h.granted();
        let mut invalid = identity(14400);
        match case {
            0 => invalid.user_id = "001".into(),
            1 => invalid.login = "INVALID".into(),
            2 => invalid.scopes.clear(),
            3 => invalid.expires_in = Duration::ZERO,
            _ => invalid.expires_in = Duration::from_secs(604801),
        }
        h.validate(invalid);
        assert_eq!(h.controller.tick(), Err(Error::InvalidResult));
        assert!(h.controller.snapshot().login.is_none());
        h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
    }
}

#[test]
fn cancellation_and_failed_disconnect_save_preserve_polling_rules() {
    let mut h = Harness::new(false);
    h.awaiting();
    let previous = h.controller.snapshot();
    h.storage.lock().unwrap().fail_save = true;
    assert_eq!(h.controller.disconnect(), Err(Error::SettingsSaveFailed));
    assert_eq!(h.controller.snapshot(), previous);
    h.clock.advance(5);
    h.tick();
    assert_eq!(h.controller.disconnect(), Err(Error::Busy));
    h.fail(TwitchDisposition::Cancelled, 0);
    assert_eq!(h.controller.tick(), Err(Error::AuthenticationCancelled));
    assert!(h.controller.snapshot().user_code.is_none());
    h.empty();
}

#[test]
fn failed_erase_during_shutdown_remains_a_visible_typed_diagnostic() {
    let mut h = Harness::new(false);
    h.connected(14400);
    h.controller.disconnect().unwrap();
    h.storage.lock().unwrap().fail_erase = true;
    h.controller.shutdown();
    assert_eq!(
        h.controller.snapshot().diagnostic,
        Some(Error::SessionEraseFailed)
    );
    assert_eq!(h.controller.snapshot().state, State::ShuttingDown);
    assert!(!h.config.lock().unwrap().snapshot().settings.twitch.enabled);
    assert_eq!(h.worker.lock().unwrap().cancellations, 1);
}

#[test]
fn an_unrepresentable_schedule_releases_the_session_transaction() {
    let mut h = Harness::new(false);
    h.granted();
    let mut latest = h.clock.steady_now();
    for bit in (0..64).rev() {
        if let Some(later) = latest.checked_add(Duration::from_secs(1_u64 << bit)) {
            latest = later;
        }
    }
    h.clock.0.lock().unwrap().0 = latest;
    h.validate(identity(14400));
    assert_eq!(h.controller.tick(), Err(Error::InvalidResult));
    h.unavailable(TwitchDeliverySessionErrorCode::NotConnected);
    assert_eq!(h.controller.snapshot().state, State::Error);
    assert!(h.storage.lock().unwrap().bytes.is_none());
}
