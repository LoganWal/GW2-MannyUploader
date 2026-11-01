use super::{
    SecretValue, TwitchCredentialSet, TwitchDeviceAuthorization, TwitchError, TwitchOperation,
    TwitchTokenGrant, TwitchValidatedIdentity,
};

#[derive(Debug)]
pub enum TwitchAuthenticationCommand {
    Start,
    Poll { device_code: SecretValue },
    Validate { credentials: TwitchCredentialSet },
    Refresh { credentials: TwitchCredentialSet },
    Revoke { access_token: SecretValue },
}
impl TwitchAuthenticationCommand {
    pub fn operation(&self) -> TwitchOperation {
        match self {
            Self::Start => TwitchOperation::Start,
            Self::Poll { .. } => TwitchOperation::Poll,
            Self::Validate { .. } => TwitchOperation::Validate,
            Self::Refresh { .. } => TwitchOperation::Refresh,
            Self::Revoke { .. } => TwitchOperation::Revoke,
        }
    }
}
#[derive(Debug)]
pub struct TwitchAuthenticationRequest {
    pub request_id: u64,
    pub command: TwitchAuthenticationCommand,
}
#[derive(Debug)]
pub enum TwitchAuthenticationSuccess {
    Started(TwitchDeviceAuthorization),
    Pending {
        device_code: SecretValue,
    },
    Granted(TwitchTokenGrant),
    Validated {
        credentials: TwitchCredentialSet,
        identity: TwitchValidatedIdentity,
    },
    Refreshed(TwitchTokenGrant),
    Revoked,
}
#[derive(Debug)]
pub enum TwitchAuthenticationSecret {
    DeviceCode(SecretValue),
    Credentials(TwitchCredentialSet),
}
#[derive(Debug)]
pub struct TwitchAuthenticationResult {
    pub request_id: u64,
    pub operation: TwitchOperation,
    pub outcome: Result<TwitchAuthenticationSuccess, TwitchError>,
    /// Failed primitives return ownership for application recovery or disposal, never diagnostics.
    pub retained: Option<TwitchAuthenticationSecret>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchAuthenticationDispatchError {
    InvalidRequest,
    QueueFull,
    Stopping,
}
pub trait TwitchAuthenticator: Send {
    fn enqueue(
        &mut self,
        request: TwitchAuthenticationRequest,
    ) -> Result<(), TwitchAuthenticationDispatchError>;
    fn try_take_result(&mut self) -> Option<TwitchAuthenticationResult>;
    fn cancel_pending(&mut self);
}
