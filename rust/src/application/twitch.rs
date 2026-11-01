use super::{HttpErrorCode, SecretValue, StopToken};
use crate::domain::TwitchDeliveryReceipt;
use std::time::Duration;

/// Runtime ownership of the public application identity. This port performs no HTTP.
pub trait TwitchApplicationConfiguration: Send + Sync {
    fn set_client_id(&self, client_id: String) -> Result<(), TwitchError>;
}

pub struct TwitchDeviceAuthorization {
    pub device_code: SecretValue,
    pub user_code: String,
    pub verification_uri: String,
    pub expires_in: Duration,
    pub polling_interval: Duration,
}
impl std::fmt::Debug for TwitchDeviceAuthorization {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("TwitchDeviceAuthorization([REDACTED])")
    }
}
#[derive(Debug)]
pub struct TwitchTokenGrant {
    pub access_token: SecretValue,
    pub refresh_token: SecretValue,
    pub expires_in: Duration,
    pub scopes: Vec<String>,
}
#[derive(Debug)]
pub enum TwitchDevicePollResult {
    Pending,
    Granted(TwitchTokenGrant),
}
pub struct TwitchValidatedIdentity {
    pub user_id: String,
    pub login: String,
    pub expires_in: Duration,
    pub scopes: Vec<String>,
}
impl std::fmt::Debug for TwitchValidatedIdentity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("TwitchValidatedIdentity([REDACTED])")
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchOperation {
    Start,
    Poll,
    Validate,
    Refresh,
    Revoke,
    SendChat,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchDisposition {
    Retry,
    Reconnect,
    Failed,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchErrorCode {
    InvalidClientId,
    Unconfigured,
    InvalidInput,
    BodyFailed,
    Transport,
    HttpStatus,
    InvalidResponse,
    Internal,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TwitchError {
    pub operation: TwitchOperation,
    pub code: TwitchErrorCode,
    pub disposition: TwitchDisposition,
    pub retry_after: Option<Duration>,
    pub http_error: Option<HttpErrorCode>,
    pub http_status: Option<u16>,
}
impl TwitchError {
    pub fn failed(operation: TwitchOperation, code: TwitchErrorCode) -> Self {
        Self {
            operation,
            code,
            disposition: TwitchDisposition::Failed,
            retry_after: None,
            http_error: None,
            http_status: None,
        }
    }
    pub fn detail(&self) -> &'static str {
        match self.disposition {
            TwitchDisposition::Retry => "Twitch is temporarily unavailable",
            TwitchDisposition::Reconnect => "Reconnect Twitch to continue",
            TwitchDisposition::Failed => "The Twitch request could not be completed",
            TwitchDisposition::Cancelled => "The Twitch request was cancelled",
        }
    }
}
impl std::fmt::Display for TwitchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for TwitchError {}

/// Synchronous primitives. Scheduling, durable token rotation, and retry policy belong to callers.
pub trait TwitchApi: Send + Sync {
    fn start_device_authorization(
        &self,
        stop: &StopToken,
    ) -> Result<TwitchDeviceAuthorization, TwitchError>;
    fn poll_device_authorization(
        &self,
        code: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchDevicePollResult, TwitchError>;
    fn validate_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchValidatedIdentity, TwitchError>;
    fn refresh_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchTokenGrant, TwitchError>;
    fn revoke_access_token(&self, token: &SecretValue, stop: &StopToken)
    -> Result<(), TwitchError>;
    fn send_chat_message(
        &self,
        authenticated_user_id: &str,
        message: &str,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchDeliveryReceipt, TwitchError>;
}

pub fn valid_twitch_client_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
}
