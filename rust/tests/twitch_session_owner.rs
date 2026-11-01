use manny_uploader::{
    application::*,
    domain::{Settings, TwitchDeliveryReceipt},
};
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex, mpsc},
    time::{Duration, Instant, SystemTime},
};

type OwnerError = TwitchSessionOwnerError;
type DeliveryCode = TwitchDeliverySessionErrorCode;
const WAIT: Duration = Duration::from_secs(5);
fn secret(bytes: &[u8]) -> SecretValue {
    SecretValue::from_bytes(bytes.to_vec())
}
fn scopes() -> Vec<String> {
    vec![TWITCH_CHAT_SCOPE.into()]
}
fn session() -> TwitchSession {
    TwitchSession {
        credentials: TwitchCredentialSet {
            access_token: secret(b"old-access"),
            refresh_token: secret(b"old-refresh"),
            access_expires_at: SystemTime::UNIX_EPOCH + Duration::from_secs(2000),
        },
        user_id: "123".into(),
        login: "broadcaster".into(),
        scopes: scopes(),
    }
}
fn identity() -> TwitchValidatedIdentity {
    TwitchValidatedIdentity {
        user_id: "123".into(),
        login: "renamed".into(),
        expires_in: Duration::from_secs(300),
        scopes: scopes(),
    }
}
fn grant() -> TwitchTokenGrant {
    TwitchTokenGrant {
        access_token: secret(b"new-access"),
        refresh_token: secret(b"new-refresh"),
        expires_in: Duration::from_secs(600),
        scopes: scopes(),
    }
}
fn api_error(disposition: TwitchDisposition) -> TwitchError {
    TwitchError {
        disposition,
        retry_after: Some(Duration::from_secs(30)),
        ..TwitchError::failed(TwitchOperation::Validate, TwitchErrorCode::HttpStatus)
    }
}
#[derive(Default)]
struct Storage {
    bytes: Option<Vec<u8>>,
    events: Vec<String>,
    fail_store: bool,
    panic_store: bool,
    fail_erase: bool,
}
struct Ordinary;
impl SettingsStore for Ordinary {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        Ok(SettingsLoadResult {
            settings: Settings::new("synthetic-logs".into()),
            source: SettingsLoadSource::Primary,
            recovery: None,
        })
    }
    fn save(&self, _: &Settings) -> Result<(), SettingsStoreError> {
        panic!("session owner must not change settings")
    }
}
struct Protected(Arc<Mutex<Storage>>);
fn storage_error() -> SecretStoreError {
    SecretStoreError {
        id: Some(SecretId::TwitchOAuthSession),
        code: SecretStoreErrorCode::NotFound,
        system_error: None,
    }
}
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
            .ok_or_else(storage_error)
    }
    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::TwitchOAuthSession);
        let decoded = decode_twitch_session(value).unwrap();
        let mut state = self.0.lock().unwrap();
        state.events.push(format!(
            "store:{}",
            String::from_utf8_lossy(decoded.credentials.access_token.expose())
        ));
        if state.panic_store {
            drop(state);
            panic!("synthetic protected adapter panic");
        }
        if state.fail_store {
            return Err(storage_error());
        }
        state.bytes = Some(value.expose().to_vec());
        Ok(())
    }
    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::TwitchOAuthSession);
        let mut state = self.0.lock().unwrap();
        state.events.push("erase".into());
        if state.fail_erase {
            return Err(storage_error());
        }
        state.bytes = None;
        Ok(())
    }
}
struct FixedClock;
impl Clock for FixedClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH + Duration::from_secs(1000)
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}
type Hook = dyn Fn(TwitchOperation, &StopToken) + Send + Sync;
struct Api {
    storage: Arc<Mutex<Storage>>,
    validations: Mutex<VecDeque<Result<TwitchValidatedIdentity, TwitchError>>>,
    refreshes: Mutex<VecDeque<Result<TwitchTokenGrant, TwitchError>>>,
    hook: Box<Hook>,
}
impl TwitchApi for Api {
    fn start_device_authorization(
        &self,
        _: &StopToken,
    ) -> Result<TwitchDeviceAuthorization, TwitchError> {
        panic!("unexpected start")
    }
    fn poll_device_authorization(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchDevicePollResult, TwitchError> {
        panic!("unexpected poll")
    }
    fn revoke_access_token(&self, _: &SecretValue, _: &StopToken) -> Result<(), TwitchError> {
        panic!("unexpected revoke")
    }
    fn send_chat_message(
        &self,
        _: &str,
        _: &str,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchDeliveryReceipt, TwitchError> {
        panic!("unexpected chat")
    }
    fn validate_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchValidatedIdentity, TwitchError> {
        let mut storage = self.storage.lock().unwrap();
        storage.events.push(format!(
            "validate:{}",
            String::from_utf8_lossy(token.expose())
        ));
        if token.expose() == b"new-access" {
            let saved = decode_twitch_session(&secret(
                storage
                    .bytes
                    .as_ref()
                    .expect("rotated record saved before validation"),
            ))
            .unwrap();
            assert_eq!(saved.credentials.access_token.expose(), b"new-access");
            assert_eq!(saved.credentials.refresh_token.expose(), b"new-refresh");
            assert_eq!(saved.user_id, "123");
        }
        drop(storage);
        (self.hook)(TwitchOperation::Validate, stop);
        self.validations
            .lock()
            .unwrap()
            .pop_front()
            .expect("unexpected validation")
    }
    fn refresh_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchTokenGrant, TwitchError> {
        assert_eq!(token.expose(), b"old-refresh");
        self.storage.lock().unwrap().events.push("refresh".into());
        (self.hook)(TwitchOperation::Refresh, stop);
        self.refreshes
            .lock()
            .unwrap()
            .pop_front()
            .expect("unexpected refresh")
    }
}
struct Harness {
    storage: Arc<Mutex<Storage>>,
    config: Arc<Mutex<ConfigurationService>>,
    api: Arc<Api>,
    owner: Arc<TwitchSessionOwner>,
    cancel: CancellationSource,
}
impl Harness {
    fn new() -> Self {
        Self::with_hook(Box::new(|_, _| {}))
    }
    fn with_hook(hook: Box<Hook>) -> Self {
        let storage = Arc::new(Mutex::new(Storage::default()));
        let config = Arc::new(Mutex::new(
            ConfigurationService::new(Box::new(Ordinary), Ok(Box::new(Protected(storage.clone()))))
                .unwrap(),
        ));
        let api = Arc::new(Api {
            storage: storage.clone(),
            validations: Mutex::new(VecDeque::new()),
            refreshes: Mutex::new(VecDeque::new()),
            hook,
        });
        let owner = Arc::new(TwitchSessionOwner::new(
            config.clone(),
            api.clone(),
            Arc::new(FixedClock),
        ));
        Self {
            storage,
            config,
            api,
            owner,
            cancel: CancellationSource::default(),
        }
    }
    fn activate(&self) {
        let transaction = self.owner.begin_new(session()).unwrap();
        self.owner
            .commit(transaction.id, transaction.session)
            .unwrap();
        self.storage.lock().unwrap().events.clear();
    }
    fn acquire(&self) -> TwitchDeliverySession {
        self.owner.acquire(&self.cancel.token()).unwrap()
    }
    fn recover(&self) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        self.owner.recover(self.acquire(), &self.cancel.token())
    }
    fn validate(&self, value: Result<TwitchValidatedIdentity, TwitchError>) {
        self.api.validations.lock().unwrap().push_back(value);
    }
    fn rotate(&self) {
        self.validate(Err(api_error(TwitchDisposition::Reconnect)));
        self.api.refreshes.lock().unwrap().push_back(Ok(grant()));
    }
    fn events(&self) -> Vec<String> {
        self.storage.lock().unwrap().events.clone()
    }
    fn unavailable(&self, code: DeliveryCode) {
        assert_eq!(
            self.owner.acquire(&self.cancel.token()).unwrap_err().code,
            code
        );
    }
}

#[test]
fn saved_identity_is_hidden_until_protected_commit() {
    let h = Harness::new();
    assert!(h.owner.checkout_saved().unwrap().is_none());
    h.unavailable(DeliveryCode::NotConnected);
    h.storage.lock().unwrap().bytes =
        Some(encode_twitch_session(&session()).unwrap().expose().to_vec());
    let tx = h.owner.checkout_saved().unwrap().unwrap();
    assert!(h.owner.snapshot().is_none());
    h.unavailable(DeliveryCode::Retry);
    assert_eq!(
        h.owner.restore(tx.id, session()),
        Err(OwnerError::StaleTransaction)
    );
    let snapshot = h.owner.commit(tx.id, tx.session).unwrap();
    assert_eq!(snapshot.revision, 1);
    assert_eq!(h.acquire().authenticated_user_id, "123");
    assert_eq!(h.events(), ["store:old-access"]);
}

#[test]
fn corrupt_records_and_failed_commits_never_publish() {
    let h = Harness::new();
    h.storage.lock().unwrap().bytes = Some(b"invalid".to_vec());
    assert_eq!(
        h.owner.checkout_saved().unwrap_err(),
        OwnerError::DecodeFailed
    );
    h.unavailable(DeliveryCode::NotConnected);
    for panic in [false, true] {
        let tx = h.owner.begin_new(session()).unwrap();
        {
            let mut store = h.storage.lock().unwrap();
            store.fail_store = true;
            store.panic_store = panic;
        }
        assert_eq!(
            h.owner.commit(tx.id, tx.session),
            Err(OwnerError::StoreFailed)
        );
        h.unavailable(DeliveryCode::Retry);
        h.owner.discard(tx.id, false).unwrap();
        h.unavailable(DeliveryCode::NotConnected);
        assert!(h.config.try_lock().is_ok());
    }
}

#[test]
fn exclusive_transactions_reject_stale_writes_and_preserve_revision_on_restore() {
    let h = Harness::new();
    h.activate();
    let rejected = h.acquire();
    let tx = h.owner.checkout_active().unwrap();
    h.unavailable(DeliveryCode::Retry);
    assert_eq!(h.owner.checkout_active().unwrap_err(), OwnerError::Busy);
    assert_eq!(
        h.owner
            .recover(rejected, &h.cancel.token())
            .unwrap_err()
            .code,
        DeliveryCode::Retry
    );
    assert_eq!(
        h.owner.persist_pending(tx.id + 1, &session()),
        Err(OwnerError::StaleTransaction)
    );
    assert_eq!(
        h.owner.commit(tx.id + 1, session()),
        Err(OwnerError::StaleTransaction)
    );
    assert_eq!(
        h.owner.discard(tx.id + 1, true),
        Err(OwnerError::StaleTransaction)
    );
    h.owner.restore(tx.id, tx.session).unwrap();
    assert_eq!(h.acquire().revision, 1);
    assert!(h.events().is_empty());
    assert_eq!(
        h.owner.restore(tx.id, session()),
        Err(OwnerError::StaleTransaction)
    );
}

#[test]
fn rotation_cannot_restore_the_old_generation_even_after_store_failure() {
    for fail in [false, true] {
        let h = Harness::new();
        h.activate();
        let tx = h.owner.checkout_active().unwrap();
        h.storage.lock().unwrap().fail_store = fail;
        assert_eq!(h.owner.persist_pending(tx.id, &tx.session).is_err(), fail);
        assert_eq!(
            h.owner.restore(tx.id, tx.session),
            Err(OwnerError::StaleTransaction)
        );
        h.unavailable(DeliveryCode::Retry);
        h.owner.discard(tx.id, false).unwrap();
        h.unavailable(DeliveryCode::NotConnected);
    }
}

#[test]
fn current_validation_commits_login_expiry_and_a_new_revision() {
    let h = Harness::new();
    h.activate();
    h.validate(Ok(identity()));
    let lease = h.recover().unwrap();
    assert_eq!(lease.revision, 2);
    assert_eq!(lease.access_token.expose(), b"old-access");
    let snapshot = h.owner.snapshot().unwrap();
    assert_eq!(snapshot.login, "renamed");
    assert_eq!(
        snapshot.access_expires_at,
        SystemTime::UNIX_EPOCH + Duration::from_secs(1300)
    );
    assert_eq!(h.events(), ["validate:old-access", "store:old-access"]);
}

#[test]
fn refresh_persists_the_complete_pair_before_replacement_validation() {
    let h = Harness::new();
    h.activate();
    h.rotate();
    h.validate(Ok(identity()));
    let stale = h.acquire();
    let lease = h.recover().unwrap();
    assert_eq!(lease.revision, 2);
    assert_eq!(lease.access_token.expose(), b"new-access");
    let newer = h.owner.recover(stale, &h.cancel.token()).unwrap();
    assert_eq!(newer.revision, 2);
    assert_eq!(newer.access_token.expose(), b"new-access");
    assert_eq!(
        h.events(),
        [
            "validate:old-access",
            "refresh",
            "store:new-access",
            "validate:new-access",
            "store:new-access"
        ]
    );
}

#[test]
fn forged_and_foreign_leases_cannot_trigger_recovery() {
    let h = Harness::new();
    h.activate();
    for case in 0..4 {
        let mut lease = h.acquire();
        match case {
            0 => lease.revision = 0,
            1 => lease.revision = 2,
            2 => lease.authenticated_user_id = "456".into(),
            _ => lease.access_token = secret(b"foreign-token"),
        }
        assert_eq!(
            h.owner.recover(lease, &h.cancel.token()).unwrap_err().code,
            DeliveryCode::ReconnectRequired
        );
    }
    assert_eq!(h.acquire().revision, 1);
    assert!(h.events().is_empty());
}

#[test]
fn old_leases_cannot_switch_to_another_broadcaster() {
    let h = Harness::new();
    h.activate();
    let old = h.acquire();
    h.owner.erase_saved().unwrap();
    let mut other = session();
    other.user_id = "456".into();
    let tx = h.owner.begin_new(other).unwrap();
    h.owner.commit(tx.id, tx.session).unwrap();
    assert_eq!(
        h.owner.recover(old, &h.cancel.token()).unwrap_err().code,
        DeliveryCode::ReconnectRequired
    );
    assert_eq!(h.acquire().authenticated_user_id, "456");
}

#[test]
fn pre_rotation_transient_failures_restore_without_writes_or_automatic_retries() {
    for refresh in [false, true] {
        for disposition in [TwitchDisposition::Retry, TwitchDisposition::Cancelled] {
            let h = Harness::new();
            h.activate();
            if refresh {
                h.validate(Err(api_error(TwitchDisposition::Reconnect)));
                h.api
                    .refreshes
                    .lock()
                    .unwrap()
                    .push_back(Err(api_error(disposition)));
            } else {
                h.validate(Err(api_error(disposition)));
            }
            let error = h.recover().unwrap_err();
            assert_eq!(
                error.code,
                if disposition == TwitchDisposition::Retry {
                    DeliveryCode::Retry
                } else {
                    DeliveryCode::Cancelled
                }
            );
            assert_eq!(error.retry_after, Some(Duration::from_secs(30)));
            assert_eq!(h.acquire().revision, 1);
            assert_eq!(h.acquire().access_token.expose(), b"old-access");
            assert_eq!(
                h.events(),
                if refresh {
                    vec!["validate:old-access", "refresh"]
                } else {
                    vec!["validate:old-access"]
                }
            );
        }
    }
}

#[test]
fn replacement_failures_erase_and_never_refresh_twice() {
    for disposition in [
        TwitchDisposition::Retry,
        TwitchDisposition::Reconnect,
        TwitchDisposition::Failed,
        TwitchDisposition::Cancelled,
    ] {
        let h = Harness::new();
        h.activate();
        h.rotate();
        h.validate(Err(api_error(disposition)));
        assert!(h.recover().is_err());
        h.unavailable(DeliveryCode::NotConnected);
        assert!(h.storage.lock().unwrap().bytes.is_none());
        assert_eq!(
            h.events(),
            [
                "validate:old-access",
                "refresh",
                "store:new-access",
                "validate:new-access",
                "erase"
            ]
        );
    }
}

#[test]
fn changed_identity_or_invalid_scope_never_publishes() {
    for rotated in [false, true] {
        for wrong_user in [false, true] {
            let h = Harness::new();
            h.activate();
            if rotated {
                h.rotate();
            }
            let mut validated = identity();
            if wrong_user {
                validated.user_id = "456".into();
            } else {
                validated.scopes.clear();
            }
            h.validate(Ok(validated));
            assert_eq!(
                h.recover().unwrap_err().code,
                DeliveryCode::ReconnectRequired
            );
            h.unavailable(DeliveryCode::NotConnected);
            assert!(h.storage.lock().unwrap().bytes.is_none());
        }
    }
}

#[test]
fn rotation_store_failure_stops_before_replacement_validation() {
    let h = Harness::new();
    h.activate();
    h.rotate();
    h.storage.lock().unwrap().fail_store = true;
    assert_eq!(h.recover().unwrap_err().code, DeliveryCode::Failed);
    h.unavailable(DeliveryCode::NotConnected);
    assert_eq!(
        h.events(),
        ["validate:old-access", "refresh", "store:new-access"]
    );
}

#[test]
fn erase_failure_is_visible_and_does_not_restore_a_session() {
    let h = Harness::new();
    h.activate();
    h.rotate();
    h.validate(Err(api_error(TwitchDisposition::Reconnect)));
    h.storage.lock().unwrap().fail_erase = true;
    assert_eq!(h.recover().unwrap_err().code, DeliveryCode::Failed);
    h.unavailable(DeliveryCode::NotConnected);
    assert!(h.storage.lock().unwrap().bytes.is_some());
}

#[test]
fn network_callbacks_hold_neither_owner_nor_configuration_lock() {
    for shutdown in [false, true] {
        let (entered_tx, entered_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let release_rx = Mutex::new(release_rx);
        let h = Harness::with_hook(Box::new(move |_, _| {
            entered_tx.send(()).unwrap();
            release_rx.lock().unwrap().recv_timeout(WAIT).unwrap();
        }));
        h.activate();
        h.validate(Ok(identity()));
        let lease = h.acquire();
        let competing = h.acquire();
        let owner = h.owner.clone();
        let stop = h.cancel.token();
        let worker = std::thread::spawn(move || owner.recover(lease, &stop));
        entered_rx.recv_timeout(WAIT).unwrap();
        h.unavailable(DeliveryCode::Retry);
        assert!(h.owner.snapshot().is_none());
        assert_eq!(h.owner.checkout_active().unwrap_err(), OwnerError::Busy);
        assert_eq!(
            h.owner
                .recover(competing, &h.cancel.token())
                .unwrap_err()
                .code,
            DeliveryCode::Retry
        );
        assert!(h.config.try_lock().is_ok());
        if shutdown {
            h.owner.shutdown();
        }
        release_tx.send(()).unwrap();
        let result = worker.join().unwrap();
        if shutdown {
            assert_eq!(result.unwrap_err().code, DeliveryCode::Cancelled);
            h.unavailable(DeliveryCode::Cancelled);
            assert_eq!(h.events(), ["validate:old-access"]);
        } else {
            assert_eq!(result.unwrap().revision, 2);
        }
    }
}

#[test]
fn cancellation_after_rotation_never_restores_old_credentials() {
    let cancel = Arc::new(CancellationSource::default());
    let hook_cancel = cancel.clone();
    let h = Harness::with_hook(Box::new(move |operation, _| {
        if operation == TwitchOperation::Refresh {
            hook_cancel.request_stop();
        }
    }));
    h.activate();
    h.rotate();
    assert_eq!(
        h.owner
            .recover(h.acquire(), &cancel.token())
            .unwrap_err()
            .code,
        DeliveryCode::Cancelled
    );
    h.unavailable(DeliveryCode::NotConnected);
    assert_eq!(h.events(), ["validate:old-access", "refresh", "erase"]);
}

#[test]
fn panics_are_contained_and_clear_the_transaction() {
    let h = Harness::with_hook(Box::new(|_, _| panic!("synthetic client panic")));
    h.activate();
    assert_eq!(h.recover().unwrap_err().code, DeliveryCode::Failed);
    h.unavailable(DeliveryCode::NotConnected);
    let tx = h.owner.begin_new(session()).unwrap();
    h.owner.discard(tx.id, false).unwrap();
}

#[test]
fn shutdown_invalidates_checked_out_state_and_rejects_late_commits() {
    let h = Harness::new();
    h.activate();
    let tx = h.owner.checkout_active().unwrap();
    h.owner.shutdown();
    h.owner.shutdown();
    assert!(h.owner.is_shutting_down());
    assert_eq!(
        h.owner.commit(tx.id, tx.session),
        Err(OwnerError::ShuttingDown)
    );
    assert_eq!(
        h.owner.checkout_saved().unwrap_err(),
        OwnerError::ShuttingDown
    );
    h.unavailable(DeliveryCode::Cancelled);
    assert!(h.events().is_empty());
}
