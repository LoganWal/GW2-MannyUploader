use std::sync::{Arc, Mutex};

use manny_uploader::application::*;
use manny_uploader::domain::Settings;

const MARKER: &[u8] = b"synthetic-configuration-secret";
fn settings() -> Settings {
    Settings::new("synthetic/logs".into())
}
fn source(source: SettingsLoadSource) -> SettingsLoadResult {
    SettingsLoadResult {
        settings: settings(),
        source,
        recovery: (source == SettingsLoadSource::Backup).then_some(SettingsReadIssue::InvalidJson),
    }
}
struct State {
    loaded: Result<SettingsLoadResult, SettingsStoreError>,
    load_count: usize,
    save_count: usize,
    save_error: Option<SettingsStoreError>,
    saved: Vec<Settings>,
    secret_error: Option<SecretStoreError>,
    secret_loads: Vec<SecretId>,
    secret_stores: Vec<SecretId>,
    secret_erases: Vec<SecretId>,
    mode: SecretProtectionMode,
}
impl Default for State {
    fn default() -> Self {
        Self {
            loaded: Ok(source(SettingsLoadSource::Defaults)),
            load_count: 0,
            save_count: 0,
            save_error: None,
            saved: vec![],
            secret_error: None,
            secret_loads: vec![],
            secret_stores: vec![],
            secret_erases: vec![],
            mode: SecretProtectionMode::NativeUserScoped,
        }
    }
}
struct Ordinary(Arc<Mutex<State>>);
impl SettingsStore for Ordinary {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        let mut state = self.0.lock().unwrap();
        state.load_count += 1;
        state.loaded.clone()
    }
    fn save(&self, candidate: &Settings) -> Result<(), SettingsStoreError> {
        let mut state = self.0.lock().unwrap();
        state.save_count += 1;
        if let Some(error) = &state.save_error {
            return Err(error.clone());
        }
        state.saved.push(candidate.clone());
        Ok(())
    }
}
struct Protected(Arc<Mutex<State>>);
impl SecretStore for Protected {
    fn mode(&self) -> SecretProtectionMode {
        self.0.lock().unwrap().mode
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        let mut state = self.0.lock().unwrap();
        state.secret_loads.push(id);
        match state.secret_error {
            Some(error) => Err(error),
            None => Ok(SecretValue::from_bytes(MARKER.to_vec())),
        }
    }
    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError> {
        assert_eq!(value.expose(), MARKER);
        let mut state = self.0.lock().unwrap();
        state.secret_stores.push(id);
        state.secret_error.map_or(Ok(()), Err)
    }
    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError> {
        let mut state = self.0.lock().unwrap();
        state.secret_erases.push(id);
        state.secret_error.map_or(Ok(()), Err)
    }
}
fn create(state: &Arc<Mutex<State>>) -> Result<ConfigurationService, ConfigurationError> {
    ConfigurationService::new(
        Box::new(Ordinary(state.clone())),
        Ok(Box::new(Protected(state.clone()))),
    )
}

#[test]
fn initialization_loads_once_preserves_source_and_returns_independent_snapshots() {
    for source_kind in [
        SettingsLoadSource::Defaults,
        SettingsLoadSource::Primary,
        SettingsLoadSource::Backup,
    ] {
        let state = Arc::new(Mutex::new(State {
            loaded: Ok(source(source_kind)),
            ..Default::default()
        }));
        let service = create(&state).unwrap();
        let snapshot = service.snapshot();
        assert_eq!(snapshot.settings, settings());
        assert_eq!(snapshot.settings_load_source, source_kind);
        assert_eq!(snapshot.settings_recovery, source(source_kind).recovery);
        assert_eq!(snapshot.revision, 1);
        assert!(!snapshot.shutting_down);
        assert_eq!(
            snapshot.persistent_secret_storage.state,
            PersistentSecretStorageState::Available
        );
        assert_eq!(
            snapshot.persistent_secret_storage.mode,
            Some(SecretProtectionMode::NativeUserScoped)
        );
        assert_eq!(snapshot.persistent_secret_storage.error_code, None);
        let mut detached = snapshot.clone();
        detached.settings.general.log_directory.clear();
        assert_eq!(service.snapshot(), snapshot);
        assert_eq!(state.lock().unwrap().load_count, 1);
    }
}

#[test]
fn settings_load_errors_and_invalid_loaded_settings_prevent_construction() {
    let failure = SettingsStoreError::NoValidSettings {
        primary: SettingsReadIssue::InvalidJson,
        backup: SettingsReadIssue::Missing,
    };
    let state = Arc::new(Mutex::new(State {
        loaded: Err(failure.clone()),
        ..Default::default()
    }));
    assert!(
        matches!(create(&state), Err(ConfigurationError::SettingsLoad(error)) if error == failure)
    );
    let mut invalid = source(SettingsLoadSource::Primary);
    invalid.settings.general.recent_log_limit = 0;
    state.lock().unwrap().loaded = Ok(invalid);
    assert!(matches!(
        create(&state),
        Err(ConfigurationError::SettingsLoad(
            SettingsStoreError::InvalidValues(_)
        ))
    ));
}

#[test]
fn unavailable_storage_is_explicit_while_wine_remains_available_with_its_mode() {
    for (code, expected) in [
        (
            SecretStoreErrorCode::Protection(SecretProtectionErrorCode::UnsupportedEnvironment),
            PersistentSecretStorageState::UnsupportedEnvironment,
        ),
        (
            SecretStoreErrorCode::File(FileError::DirectoryCreateFailed),
            PersistentSecretStorageState::InitializationFailed,
        ),
    ] {
        let state = Arc::new(Mutex::new(State::default()));
        let mut service = ConfigurationService::new(
            Box::new(Ordinary(state.clone())),
            Err(SecretStoreError {
                id: None,
                code,
                system_error: Some(50),
            }),
        )
        .unwrap();
        let capability = service.snapshot().persistent_secret_storage;
        assert_eq!(capability.state, expected);
        assert_eq!(capability.mode, None);
        assert_eq!(capability.error_code, Some(code));
        assert_eq!(capability.system_error, Some(50));
        for id in SecretId::ALL {
            let expected = ConfigurationError::SecretStorageUnavailable(SecretStoreError {
                id: Some(id),
                code,
                system_error: Some(50),
            });
            assert_eq!(service.load_secret(id).unwrap_err(), expected);
            assert_eq!(
                service.store_secret(id, &SecretValue::from_bytes(MARKER.to_vec())),
                Err(expected.clone())
            );
            assert_eq!(service.erase_secret(id), Err(expected));
        }
        service.save_settings(settings()).unwrap();
        assert_eq!(service.snapshot().revision, 2);
        assert_eq!(service.snapshot().persistent_secret_storage, capability);
        assert!(state.lock().unwrap().secret_loads.is_empty());
    }
    let state = Arc::new(Mutex::new(State {
        mode: SecretProtectionMode::WineCompatibility,
        ..Default::default()
    }));
    let service = create(&state).unwrap();
    assert_eq!(
        service.snapshot().persistent_secret_storage.mode,
        Some(SecretProtectionMode::WineCompatibility)
    );
    assert_eq!(
        service.snapshot().persistent_secret_storage.state,
        PersistentSecretStorageState::Available
    );
}

#[test]
fn durable_save_success_commits_once_and_failure_preserves_the_complete_snapshot() {
    let state = Arc::new(Mutex::new(State {
        loaded: Ok(source(SettingsLoadSource::Backup)),
        ..Default::default()
    }));
    let mut service = create(&state).unwrap();
    let before = service.snapshot();
    state.lock().unwrap().save_error = Some(SettingsStoreError::File(FileError::ReplaceFailed));
    let mut candidate = settings();
    candidate.general.recent_log_limit = 42;
    assert_eq!(
        service.save_settings(candidate.clone()),
        Err(ConfigurationError::SettingsSave(SettingsStoreError::File(
            FileError::ReplaceFailed
        )))
    );
    assert_eq!(service.snapshot(), before);
    state.lock().unwrap().save_error = None;
    service.save_settings(candidate.clone()).unwrap();
    assert_eq!(state.lock().unwrap().saved, vec![candidate.clone()]);
    let after = service.snapshot();
    assert_eq!(after.settings, candidate);
    assert_eq!(after.settings_load_source, SettingsLoadSource::Primary);
    assert_eq!(after.settings_recovery, None);
    assert_eq!(after.revision, 2);
    candidate.general.recent_log_limit = 0;
    assert!(matches!(
        service.save_settings(candidate),
        Err(ConfigurationError::SettingsSave(
            SettingsStoreError::InvalidValues(_)
        ))
    ));
    assert_eq!(service.snapshot(), after);
    assert_eq!(state.lock().unwrap().save_count, 2);
}

#[test]
fn credential_operations_route_ids_without_changing_or_contaminating_snapshots() {
    let state = Arc::new(Mutex::new(State::default()));
    let mut service = create(&state).unwrap();
    let before = service.snapshot();
    let value = SecretValue::from_bytes(MARKER.to_vec());
    for id in SecretId::ALL {
        assert_eq!(service.load_secret(id).unwrap().expose(), MARKER);
        service.store_secret(id, &value).unwrap();
        service.erase_secret(id).unwrap();
    }
    assert_eq!(state.lock().unwrap().secret_loads, SecretId::ALL);
    assert_eq!(state.lock().unwrap().secret_stores, SecretId::ALL);
    assert_eq!(state.lock().unwrap().secret_erases, SecretId::ALL);
    assert_eq!(service.snapshot(), before);
    assert!(!format!("{:?}", service.snapshot()).contains(std::str::from_utf8(MARKER).unwrap()));
    let failure = SecretStoreError {
        id: None,
        code: SecretStoreErrorCode::Protection(SecretProtectionErrorCode::ProtectionFailed),
        system_error: Some(5),
    };
    state.lock().unwrap().secret_error = Some(failure);
    let id = SecretId::TwitchOAuthSession;
    let expected = SecretStoreError {
        id: Some(id),
        ..failure
    };
    assert_eq!(
        service.load_secret(id).unwrap_err(),
        ConfigurationError::SecretLoad(expected)
    );
    assert_eq!(
        service.store_secret(id, &value),
        Err(ConfigurationError::SecretStore(expected))
    );
    assert_eq!(
        service.erase_secret(id),
        Err(ConfigurationError::SecretErase(expected))
    );
    assert_eq!(service.snapshot(), before);
}

#[test]
fn shutdown_is_idempotent_and_prevents_every_adapter_operation() {
    let state = Arc::new(Mutex::new(State::default()));
    let mut service = create(&state).unwrap();
    service.shutdown();
    let stopped = service.snapshot();
    service.shutdown();
    assert_eq!(service.snapshot(), stopped);
    assert!(service.is_shutting_down());
    assert_eq!(stopped.revision, 2);
    assert_eq!(
        service.save_settings(settings()),
        Err(ConfigurationError::ShuttingDown { secret_id: None })
    );
    for id in SecretId::ALL {
        let expected = ConfigurationError::ShuttingDown {
            secret_id: Some(id),
        };
        assert_eq!(service.load_secret(id).unwrap_err(), expected);
        assert_eq!(
            service.store_secret(id, &SecretValue::from_bytes(MARKER.to_vec())),
            Err(expected.clone())
        );
        assert_eq!(service.erase_secret(id), Err(expected));
    }
    let state = state.lock().unwrap();
    assert_eq!(state.load_count, 1);
    assert_eq!(state.save_count, 0);
    assert!(
        state.secret_loads.is_empty()
            && state.secret_stores.is_empty()
            && state.secret_erases.is_empty()
    );
}
