use std::path::{Path, PathBuf};

use super::{MAX_SETTINGS_BYTES, SettingsDocumentError, decode_settings, encode_settings};
use crate::application::{
    FileError, FileStore, SettingsLoadResult, SettingsLoadSource, SettingsReadIssue, SettingsStore,
    SettingsStoreError, validate_storage_path,
};
use crate::domain::Settings;

pub struct JsonSettingsStore {
    files: Box<dyn FileStore>,
    primary: PathBuf,
    backup: PathBuf,
    defaults: Settings,
}

impl JsonSettingsStore {
    pub fn new(
        files: Box<dyn FileStore>,
        primary: PathBuf,
        defaults: Settings,
    ) -> Result<Self, SettingsStoreError> {
        validate_storage_path(&primary).map_err(|_| SettingsStoreError::InvalidConfiguration)?;
        let errors = defaults.validate();
        if !errors.is_empty() {
            return Err(SettingsStoreError::InvalidValues(errors));
        }
        let mut backup = primary.as_os_str().to_os_string();
        backup.push(".bak");
        Ok(Self {
            files,
            primary,
            backup: backup.into(),
            defaults,
        })
    }

    pub fn primary_path(&self) -> &Path {
        &self.primary
    }
    pub fn backup_path(&self) -> &Path {
        &self.backup
    }

    fn read(&self, path: &Path) -> Result<(Settings, Vec<u8>), SettingsReadIssue> {
        let document = self
            .files
            .read(path, MAX_SETTINGS_BYTES)
            .map_err(|error| {
                if error == FileError::TooLarge {
                    SettingsReadIssue::TooLarge
                } else {
                    SettingsReadIssue::File(error)
                }
            })?
            .ok_or(SettingsReadIssue::Missing)?;
        let settings = decode_settings(&document).map_err(|error| match error {
            SettingsDocumentError::TooLarge => SettingsReadIssue::TooLarge,
            SettingsDocumentError::InvalidSettings(errors) => {
                SettingsReadIssue::InvalidValues(errors)
            }
            SettingsDocumentError::InvalidJson | SettingsDocumentError::SerializationFailed => {
                SettingsReadIssue::InvalidJson
            }
        })?;
        Ok((settings, document))
    }
}

impl SettingsStore for JsonSettingsStore {
    fn load(&self) -> Result<SettingsLoadResult, SettingsStoreError> {
        let primary_issue = match self.read(&self.primary) {
            Ok((settings, _)) => {
                return Ok(SettingsLoadResult {
                    settings,
                    source: SettingsLoadSource::Primary,
                    recovery: None,
                });
            }
            Err(issue) => issue,
        };
        match self.read(&self.backup) {
            Ok((settings, _)) => Ok(SettingsLoadResult {
                settings,
                source: SettingsLoadSource::Backup,
                recovery: Some(primary_issue),
            }),
            Err(SettingsReadIssue::Missing) if primary_issue == SettingsReadIssue::Missing => {
                Ok(SettingsLoadResult {
                    settings: self.defaults.clone(),
                    source: SettingsLoadSource::Defaults,
                    recovery: None,
                })
            }
            Err(backup) => Err(SettingsStoreError::NoValidSettings {
                primary: primary_issue,
                backup,
            }),
        }
    }

    fn save(&self, settings: &Settings) -> Result<(), SettingsStoreError> {
        let document = encode_settings(settings).map_err(|error| match error {
            SettingsDocumentError::TooLarge => SettingsStoreError::TooLarge,
            SettingsDocumentError::InvalidSettings(errors) => {
                SettingsStoreError::InvalidValues(errors)
            }
            SettingsDocumentError::InvalidJson | SettingsDocumentError::SerializationFailed => {
                SettingsStoreError::SerializationFailed
            }
        })?;
        match self.read(&self.primary) {
            Ok((_, previous)) => self
                .files
                .write_atomic(&self.backup, &previous)
                .map_err(SettingsStoreError::File)?,
            Err(SettingsReadIssue::File(error)) => return Err(SettingsStoreError::File(error)),
            Err(
                SettingsReadIssue::Missing
                | SettingsReadIssue::InvalidJson
                | SettingsReadIssue::InvalidValues(_)
                | SettingsReadIssue::TooLarge,
            ) => {}
        }
        self.files
            .write_atomic(&self.primary, &document)
            .map_err(SettingsStoreError::File)
    }
}
