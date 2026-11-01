use super::{
    SecretId, SecretProtectionErrorCode, SecretProtectionMode, SecretStore, SecretStoreError,
    SecretStoreErrorCode, SecretValue, SettingsLoadSource, SettingsReadIssue, SettingsStore,
    SettingsStoreError,
};
use crate::domain::Settings;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PersistentSecretStorageState {
    Available,
    UnsupportedEnvironment,
    InitializationFailed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PersistentSecretStorageSnapshot {
    pub state: PersistentSecretStorageState,
    pub mode: Option<SecretProtectionMode>,
    pub error_code: Option<SecretStoreErrorCode>,
    pub system_error: Option<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfigurationSnapshot {
    pub settings: Settings,
    pub settings_load_source: SettingsLoadSource,
    pub settings_recovery: Option<SettingsReadIssue>,
    pub persistent_secret_storage: PersistentSecretStorageSnapshot,
    pub revision: u64,
    pub shutting_down: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ConfigurationError {
    SettingsLoad(SettingsStoreError),
    SettingsSave(SettingsStoreError),
    SecretStorageUnavailable(SecretStoreError),
    SecretLoad(SecretStoreError),
    SecretStore(SecretStoreError),
    SecretErase(SecretStoreError),
    ShuttingDown { secret_id: Option<SecretId> },
    RevisionExhausted,
}

/// Synchronous application-owner service. UI consumers receive published copies of its snapshot.
pub struct ConfigurationService {
    settings: Box<dyn SettingsStore>,
    secrets: Option<Box<dyn SecretStore>>,
    snapshot: ConfigurationSnapshot,
}

impl ConfigurationService {
    /// An absent secret adapter requires an explicit typed initialization failure.
    pub fn new(
        settings: Box<dyn SettingsStore>,
        secrets: Result<Box<dyn SecretStore>, SecretStoreError>,
    ) -> Result<Self, ConfigurationError> {
        let loaded = settings.load().map_err(ConfigurationError::SettingsLoad)?;
        validate(&loaded.settings).map_err(ConfigurationError::SettingsLoad)?;
        let (secrets, capability) = match secrets {
            Ok(store) => {
                let mode = store.mode();
                (
                    Some(store),
                    PersistentSecretStorageSnapshot {
                        state: PersistentSecretStorageState::Available,
                        mode: Some(mode),
                        error_code: None,
                        system_error: None,
                    },
                )
            }
            Err(error) => (
                None,
                PersistentSecretStorageSnapshot {
                    state: if error.code
                        == SecretStoreErrorCode::Protection(
                            SecretProtectionErrorCode::UnsupportedEnvironment,
                        ) {
                        PersistentSecretStorageState::UnsupportedEnvironment
                    } else {
                        PersistentSecretStorageState::InitializationFailed
                    },
                    mode: None,
                    error_code: Some(error.code),
                    system_error: error.system_error,
                },
            ),
        };
        Ok(Self {
            settings,
            secrets,
            snapshot: ConfigurationSnapshot {
                settings: loaded.settings,
                settings_load_source: loaded.source,
                settings_recovery: loaded.recovery,
                persistent_secret_storage: capability,
                revision: 1,
                shutting_down: false,
            },
        })
    }

    pub fn snapshot(&self) -> ConfigurationSnapshot {
        self.snapshot.clone()
    }
    pub fn is_shutting_down(&self) -> bool {
        self.snapshot.shutting_down
    }

    pub fn save_settings(&mut self, settings: Settings) -> Result<(), ConfigurationError> {
        self.require_running(None)?;
        // Reserve the final revision for shutdown, even if saving can no longer advance.
        let revision = self
            .snapshot
            .revision
            .checked_add(1)
            .filter(|&r| r < u64::MAX)
            .ok_or(ConfigurationError::RevisionExhausted)?;
        validate(&settings).map_err(ConfigurationError::SettingsSave)?;
        self.settings
            .save(&settings)
            .map_err(ConfigurationError::SettingsSave)?;
        self.snapshot.settings = settings;
        self.snapshot.settings_load_source = SettingsLoadSource::Primary;
        self.snapshot.settings_recovery = None;
        self.snapshot.revision = revision;
        Ok(())
    }

    pub fn load_secret(&self, id: SecretId) -> Result<SecretValue, ConfigurationError> {
        self.require_running(Some(id))?;
        let store = self.secrets.as_ref().ok_or_else(|| self.unavailable(id))?;
        store
            .load(id)
            .map_err(|e| ConfigurationError::SecretLoad(request_error(id, e)))
    }
    pub fn store_secret(
        &mut self,
        id: SecretId,
        value: &SecretValue,
    ) -> Result<(), ConfigurationError> {
        self.require_running(Some(id))?;
        if self.secrets.is_none() {
            return Err(self.unavailable(id));
        }
        self.secrets
            .as_mut()
            .expect("available store")
            .store(id, value)
            .map_err(|e| ConfigurationError::SecretStore(request_error(id, e)))
    }
    pub fn erase_secret(&mut self, id: SecretId) -> Result<(), ConfigurationError> {
        self.require_running(Some(id))?;
        if self.secrets.is_none() {
            return Err(self.unavailable(id));
        }
        self.secrets
            .as_mut()
            .expect("available store")
            .erase(id)
            .map_err(|e| ConfigurationError::SecretErase(request_error(id, e)))
    }

    pub fn shutdown(&mut self) {
        if !self.snapshot.shutting_down {
            self.snapshot.shutting_down = true;
            self.snapshot.revision += 1;
        }
    }

    fn require_running(&self, secret_id: Option<SecretId>) -> Result<(), ConfigurationError> {
        if self.snapshot.shutting_down {
            Err(ConfigurationError::ShuttingDown { secret_id })
        } else {
            Ok(())
        }
    }
    fn unavailable(&self, id: SecretId) -> ConfigurationError {
        let capability = self.snapshot.persistent_secret_storage;
        ConfigurationError::SecretStorageUnavailable(SecretStoreError {
            id: Some(id),
            code: capability
                .error_code
                .expect("absent adapter has an explicit error"),
            system_error: capability.system_error,
        })
    }
}

fn request_error(id: SecretId, mut error: SecretStoreError) -> SecretStoreError {
    error.id = Some(id);
    error
}
fn validate(settings: &Settings) -> Result<(), SettingsStoreError> {
    let errors = settings.validate();
    if errors.is_empty() {
        Ok(())
    } else {
        Err(SettingsStoreError::InvalidValues(errors))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::SettingsLoadResult;
    struct SettingsAdapter;
    impl SettingsStore for SettingsAdapter {
        fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
            Ok(SettingsLoadResult {
                settings: Settings::new("logs".into()),
                source: SettingsLoadSource::Defaults,
                recovery: None,
            })
        }
        fn save(&self, _: &Settings) -> Result<(), SettingsStoreError> {
            panic!("exhausted revision must prevent writes")
        }
    }
    #[test]
    fn revision_exhaustion_prevents_writes_and_preserves_final_shutdown_transition() {
        let mut service = ConfigurationService::new(
            Box::new(SettingsAdapter),
            Err(SecretStoreError {
                id: None,
                code: SecretStoreErrorCode::InvalidConfiguration,
                system_error: None,
            }),
        )
        .unwrap();
        service.snapshot.revision = u64::MAX - 1;
        let previous = service.snapshot();
        assert_eq!(
            service.save_settings(previous.settings.clone()),
            Err(ConfigurationError::RevisionExhausted)
        );
        assert_eq!(service.snapshot(), previous);
        service.shutdown();
        service.shutdown();
        assert_eq!(service.snapshot().revision, u64::MAX);
        assert!(service.is_shutting_down());
    }
}
