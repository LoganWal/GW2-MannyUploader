use std::path::{Path, PathBuf};

use crate::application::*;

const MAGIC: &[u8; 8] = b"MNYSECR1";
const HEADER_BYTES: usize = 20;
const CHECKSUM_BYTES: usize = 4;

pub struct ProtectedFileSecretStore {
    files: Box<dyn FileStore>,
    protector: Box<dyn SecretProtector>,
    directory: PathBuf,
}

impl ProtectedFileSecretStore {
    pub fn new(
        files: Box<dyn FileStore>,
        protector: Box<dyn SecretProtector>,
        directory: PathBuf,
    ) -> Result<Self, SecretStoreError> {
        if directory.as_os_str().is_empty() || directory.as_os_str().as_encoded_bytes().contains(&0)
        {
            return Err(SecretStoreError {
                id: None,
                code: SecretStoreErrorCode::InvalidConfiguration,
                system_error: None,
            });
        }
        Ok(Self {
            files,
            protector,
            directory,
        })
    }
    pub fn directory(&self) -> &Path {
        &self.directory
    }
    pub fn record_path(&self, id: SecretId) -> PathBuf {
        self.directory.join(match id {
            SecretId::DpsReportUserToken => "dps-report-token.bin",
            SecretId::DonBotGw2ApiKey => "donbot-gw2-api-key.bin",
            SecretId::TwitchOAuthSession => "twitch-oauth-session.bin",
        })
    }
}

impl SecretStore for ProtectedFileSecretStore {
    fn mode(&self) -> SecretProtectionMode {
        self.protector.mode()
    }

    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        let record = self
            .files
            .read(&self.record_path(id), MAX_PROTECTED_RECORD_BYTES)
            .map_err(|e| file_error(id, e))?
            .ok_or(error(id, SecretStoreErrorCode::NotFound))?;
        if record.is_empty() {
            return Err(error(id, SecretStoreErrorCode::CorruptRecord));
        }
        if record.len() > MAX_PROTECTED_RECORD_BYTES {
            return Err(error(id, SecretStoreErrorCode::FileTooLarge));
        }
        let envelope = self
            .protector
            .unprotect(&record)
            .map_err(|e| protection_error(id, e))?;
        decode_envelope(id, envelope)
    }

    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError> {
        if value.is_empty() {
            return Err(error(id, SecretStoreErrorCode::EmptySecret));
        }
        if value.len() > MAX_SECRET_BYTES {
            return Err(error(id, SecretStoreErrorCode::SecretTooLarge));
        }
        let envelope = encode_envelope(id, value);
        let record = self
            .protector
            .protect(envelope.expose())
            .map_err(|e| protection_error(id, e))?;
        if record.is_empty() || record.len() > MAX_PROTECTED_RECORD_BYTES {
            return Err(error(id, SecretStoreErrorCode::FileTooLarge));
        }
        self.files
            .write_atomic(&self.record_path(id), &record)
            .map_err(|e| file_error(id, e))
    }

    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError> {
        self.files
            .erase(&self.record_path(id))
            .map_err(|e| file_error(id, e))
    }
}

fn encode_envelope(id: SecretId, value: &SecretValue) -> SecretValue {
    // Allocate its complete length before copying plaintext, so no growth can leave an old copy.
    let mut envelope =
        SecretValue::from_bytes(vec![0; HEADER_BYTES + value.len() + CHECKSUM_BYTES]);
    let bytes = envelope.expose_mut();
    bytes[..8].copy_from_slice(MAGIC);
    bytes[8..12].copy_from_slice(&1_u32.to_le_bytes());
    bytes[12..16].copy_from_slice(&(id as u32).to_le_bytes());
    bytes[16..20].copy_from_slice(&(value.len() as u32).to_le_bytes());
    let end = HEADER_BYTES + value.len();
    bytes[HEADER_BYTES..end].copy_from_slice(value.expose());
    let checksum = crc32fast::hash(&bytes[..end]);
    bytes[end..].copy_from_slice(&checksum.to_le_bytes());
    envelope
}

fn decode_envelope(id: SecretId, envelope: SecretValue) -> Result<SecretValue, SecretStoreError> {
    let corrupt = || error(id, SecretStoreErrorCode::CorruptRecord);
    let bytes = envelope.expose();
    if !(HEADER_BYTES + CHECKSUM_BYTES + 1..=HEADER_BYTES + CHECKSUM_BYTES + MAX_SECRET_BYTES)
        .contains(&bytes.len())
    {
        return Err(corrupt());
    }
    let number = |start: usize| {
        u32::from_le_bytes(
            bytes[start..start + 4]
                .try_into()
                .expect("validated envelope offsets"),
        )
    };
    if &bytes[..8] != MAGIC || number(8) != 1 || number(12) != id as u32 {
        return Err(corrupt());
    }
    let payload = number(16) as usize;
    if !(1..=MAX_SECRET_BYTES).contains(&payload)
        || payload != bytes.len() - HEADER_BYTES - CHECKSUM_BYTES
    {
        return Err(corrupt());
    }
    let end = bytes.len() - CHECKSUM_BYTES;
    if number(end) != crc32fast::hash(&bytes[..end]) {
        return Err(corrupt());
    }
    Ok(SecretValue::from_bytes(bytes[HEADER_BYTES..end].to_vec()))
}

fn error(id: SecretId, code: SecretStoreErrorCode) -> SecretStoreError {
    SecretStoreError {
        id: Some(id),
        code,
        system_error: None,
    }
}
fn file_error(id: SecretId, error_value: FileError) -> SecretStoreError {
    error(
        id,
        if error_value == FileError::TooLarge {
            SecretStoreErrorCode::FileTooLarge
        } else {
            SecretStoreErrorCode::File(error_value)
        },
    )
}
fn protection_error(id: SecretId, source: SecretProtectionError) -> SecretStoreError {
    SecretStoreError {
        id: Some(id),
        code: SecretStoreErrorCode::Protection(source.code),
        system_error: source.system_error,
    }
}
