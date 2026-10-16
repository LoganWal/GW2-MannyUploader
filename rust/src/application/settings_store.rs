use super::FileError;
use crate::domain::{Settings, SettingsValidationError};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SettingsLoadSource {
    Defaults,
    Primary,
    Backup,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SettingsReadIssue {
    Missing,
    InvalidJson,
    InvalidValues(Vec<SettingsValidationError>),
    TooLarge,
    File(FileError),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SettingsStoreError {
    InvalidConfiguration,
    InvalidValues(Vec<SettingsValidationError>),
    TooLarge,
    SerializationFailed,
    File(FileError),
    NoValidSettings {
        primary: SettingsReadIssue,
        backup: SettingsReadIssue,
    },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SettingsLoadResult {
    pub settings: Settings,
    pub source: SettingsLoadSource,
    pub recovery: Option<SettingsReadIssue>,
}

pub trait SettingsStore: Send {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError>;
    fn save(&self, settings: &Settings) -> Result<(), SettingsStoreError>;
}
