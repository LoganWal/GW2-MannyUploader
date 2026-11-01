use std::fmt;

use zeroize::{Zeroize, Zeroizing};

use super::FileError;

pub const MAX_SECRET_BYTES: usize = 16 * 1024;
pub const MAX_PROTECTION_PLAINTEXT_BYTES: usize = 32 * 1024;
pub const MAX_PROTECTED_RECORD_BYTES: usize = 64 * 1024;

/// Owned bytes with explicit exposure and wiping on clear/drop. No Clone, Display, or serialization.
///
/// ```compile_fail
/// use manny_uploader::application::SecretValue;
/// let secret = SecretValue::from_bytes(vec![1, 2, 3]);
/// let copy = secret.clone();
/// ```
#[derive(Default)]
pub struct SecretValue(Zeroizing<Vec<u8>>);

impl SecretValue {
    /// Adopts the allocation. Callers remain responsible for any earlier copies or reallocations.
    pub fn from_bytes(bytes: Vec<u8>) -> Self {
        Self(Zeroizing::new(bytes))
    }
    pub fn expose(&self) -> &[u8] {
        &self.0
    }
    pub fn len(&self) -> usize {
        self.0.len()
    }
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
    pub fn clear(&mut self) {
        self.0.zeroize();
    }
    pub(crate) fn expose_mut(&mut self) -> &mut [u8] {
        &mut self.0
    }
}

impl fmt::Debug for SecretValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("SecretValue([REDACTED])")
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum SecretId {
    DpsReportUserToken = 1,
    DonBotGw2ApiKey = 2,
    TwitchOAuthSession = 3,
}
impl SecretId {
    pub const ALL: [Self; 3] = [
        Self::DpsReportUserToken,
        Self::DonBotGw2ApiKey,
        Self::TwitchOAuthSession,
    ];
}
impl TryFrom<u32> for SecretId {
    type Error = SecretStoreError;
    fn try_from(value: u32) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::DpsReportUserToken),
            2 => Ok(Self::DonBotGw2ApiKey),
            3 => Ok(Self::TwitchOAuthSession),
            _ => Err(SecretStoreError {
                id: None,
                code: SecretStoreErrorCode::InvalidId,
                system_error: None,
            }),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecretProtectionMode {
    NativeUserScoped,
    WineCompatibility,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecretProtectionErrorCode {
    UnsupportedEnvironment,
    InputTooLarge,
    ProtectionFailed,
    UnprotectionFailed,
    OutputTooLarge,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SecretProtectionError {
    pub code: SecretProtectionErrorCode,
    pub system_error: Option<u32>,
}

pub trait SecretProtector: Send {
    fn mode(&self) -> SecretProtectionMode;
    fn protect(&self, plaintext: &[u8]) -> Result<Vec<u8>, SecretProtectionError>;
    fn unprotect(&self, ciphertext: &[u8]) -> Result<SecretValue, SecretProtectionError>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecretStoreErrorCode {
    InvalidConfiguration,
    InvalidId,
    EmptySecret,
    SecretTooLarge,
    NotFound,
    CorruptRecord,
    FileTooLarge,
    Protection(SecretProtectionErrorCode),
    File(FileError),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SecretStoreError {
    pub id: Option<SecretId>,
    pub code: SecretStoreErrorCode,
    pub system_error: Option<u32>,
}
impl fmt::Display for SecretStoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for SecretStoreError {}

/// One application owner, outside render callbacks. No plaintext fallback or backup storage.
pub trait SecretStore: Send {
    fn mode(&self) -> SecretProtectionMode;
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError>;
    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError>;
    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError>;
}
