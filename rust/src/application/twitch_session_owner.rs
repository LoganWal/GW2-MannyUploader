mod recovery;

use super::*;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex, MutexGuard},
    time::{Duration, SystemTime},
};

pub struct TwitchDeliverySession {
    pub access_token: SecretValue,
    pub authenticated_user_id: String,
    pub revision: u64,
}
impl std::fmt::Debug for TwitchDeliverySession {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("TwitchDeliverySession([REDACTED])")
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchDeliverySessionErrorCode {
    NotConnected,
    Retry,
    ReconnectRequired,
    Failed,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TwitchDeliverySessionError {
    pub code: TwitchDeliverySessionErrorCode,
    pub retry_after: Option<Duration>,
}
impl TwitchDeliverySessionError {
    pub fn detail(self) -> &'static str {
        match self.code {
            TwitchDeliverySessionErrorCode::NotConnected => "No Twitch session is connected",
            TwitchDeliverySessionErrorCode::Retry => "The Twitch session is being updated",
            TwitchDeliverySessionErrorCode::ReconnectRequired => "Reconnect Twitch to continue",
            TwitchDeliverySessionErrorCode::Failed => "The Twitch session could not be updated",
            TwitchDeliverySessionErrorCode::Cancelled => "Twitch session access was cancelled",
        }
    }
}
pub trait TwitchDeliverySessionAccess: Send + Sync {
    fn acquire(
        &self,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError>;
    fn recover(
        &self,
        rejected: TwitchDeliverySession,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError>;
}

#[derive(Debug)]
pub struct TwitchSessionTransaction {
    pub id: u64,
    pub session: TwitchSession,
}
/// Internal application metadata. Public connection snapshots must omit the user ID.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchSessionOwnerSnapshot {
    pub authenticated_user_id: String,
    pub login: String,
    pub access_expires_at: SystemTime,
    pub revision: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchSessionOwnerError {
    Busy,
    NotConnected,
    LoadFailed,
    DecodeFailed,
    EncodeFailed,
    StoreFailed,
    EraseFailed,
    StaleTransaction,
    IdentityMismatch,
    ShuttingDown,
    RevisionExhausted,
    Internal,
}
impl TwitchSessionOwnerError {
    pub fn detail(self) -> &'static str {
        match self {
            Self::Busy => "The Twitch session is already being updated",
            Self::NotConnected => "No Twitch session is connected",
            Self::LoadFailed => "The saved Twitch session could not be loaded",
            Self::DecodeFailed => "The saved Twitch session is invalid",
            Self::EncodeFailed => "The Twitch session could not be encoded safely",
            Self::StoreFailed => "The Twitch session could not be saved",
            Self::EraseFailed => "The saved Twitch session could not be removed",
            Self::StaleTransaction => "A stale Twitch session transaction was ignored",
            Self::IdentityMismatch => "The Twitch session changed broadcaster identity",
            Self::ShuttingDown => "Twitch session ownership is shutting down",
            Self::RevisionExhausted => "Twitch session ownership cannot accept more changes",
            Self::Internal => "The Twitch session could not be updated",
        }
    }
}
struct Pending {
    id: u64,
    user_id: String,
    restorable: bool,
}
struct State {
    active: Option<TwitchSession>,
    pending: Option<Pending>,
    revision: u64,
    next_transaction: u64,
    stopping: bool,
}
impl State {
    fn idle(&self) -> Result<(), TwitchSessionOwnerError> {
        if self.stopping {
            return Err(TwitchSessionOwnerError::ShuttingDown);
        }
        if self.pending.is_some() {
            return Err(TwitchSessionOwnerError::Busy);
        }
        if self.next_transaction == u64::MAX || self.revision == u64::MAX {
            return Err(TwitchSessionOwnerError::RevisionExhausted);
        }
        Ok(())
    }
    fn start(&mut self, session: TwitchSession, restorable: bool) -> TwitchSessionTransaction {
        let id = self.next_transaction;
        self.next_transaction += 1;
        self.pending = Some(Pending {
            id,
            user_id: session.user_id.clone(),
            restorable,
        });
        TwitchSessionTransaction { id, session }
    }
    fn check(&self, id: u64) -> Result<&Pending, TwitchSessionOwnerError> {
        if self.stopping {
            return Err(TwitchSessionOwnerError::ShuttingDown);
        }
        self.pending
            .as_ref()
            .filter(|pending| pending.id == id)
            .ok_or(TwitchSessionOwnerError::StaleTransaction)
    }
    fn check_identity(
        &self,
        id: u64,
        session: &TwitchSession,
    ) -> Result<(), TwitchSessionOwnerError> {
        if self.check(id)?.user_id != session.user_id {
            return Err(TwitchSessionOwnerError::IdentityMismatch);
        }
        Ok(())
    }
}

/// Lock order is session state, then configuration. Callers must release configuration guards
/// before entering this owner. Neither lock is held across HTTP or clock callbacks.
pub struct TwitchSessionOwner {
    configuration: Arc<Mutex<ConfigurationService>>,
    client: Arc<dyn TwitchApi>,
    clock: Arc<dyn Clock + Send + Sync>,
    state: Mutex<State>,
}
impl TwitchSessionOwner {
    pub fn new(
        configuration: Arc<Mutex<ConfigurationService>>,
        client: Arc<dyn TwitchApi>,
        clock: Arc<dyn Clock + Send + Sync>,
    ) -> Self {
        Self {
            configuration,
            client,
            clock,
            state: Mutex::new(State {
                active: None,
                pending: None,
                revision: 0,
                next_transaction: 1,
                stopping: false,
            }),
        }
    }
    fn lock(&self) -> Result<MutexGuard<'_, State>, TwitchSessionOwnerError> {
        self.state
            .lock()
            .map_err(|_| TwitchSessionOwnerError::Internal)
    }
    fn configuration<T>(
        &self,
        failure: TwitchSessionOwnerError,
        call: impl FnOnce(&mut ConfigurationService) -> Result<T, ConfigurationError>,
    ) -> Result<T, TwitchSessionOwnerError> {
        let mut configuration = self.configuration.lock().map_err(|_| failure)?;
        if configuration.is_shutting_down() {
            return Err(TwitchSessionOwnerError::ShuttingDown);
        }
        catch_unwind(AssertUnwindSafe(|| call(&mut configuration)))
            .map_err(|_| failure)?
            .map_err(|_| failure)
    }
    fn persist(&self, session: &TwitchSession) -> Result<(), TwitchSessionOwnerError> {
        let encoded =
            encode_twitch_session(session).map_err(|_| TwitchSessionOwnerError::EncodeFailed)?;
        self.configuration(TwitchSessionOwnerError::StoreFailed, |configuration| {
            configuration.store_secret(SecretId::TwitchOAuthSession, &encoded)
        })
    }
    fn erase(&self) -> Result<(), TwitchSessionOwnerError> {
        self.configuration(TwitchSessionOwnerError::EraseFailed, |configuration| {
            configuration.erase_secret(SecretId::TwitchOAuthSession)
        })
    }

    pub fn checkout_saved(
        &self,
    ) -> Result<Option<TwitchSessionTransaction>, TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.idle()?;
        if state.active.is_some() {
            return Err(TwitchSessionOwnerError::Busy);
        }
        let encoded = self.configuration(TwitchSessionOwnerError::LoadFailed, |configuration| {
            match configuration.load_secret(SecretId::TwitchOAuthSession) {
                Ok(value) => Ok(Some(value)),
                Err(ConfigurationError::SecretLoad(error))
                    if error.code == SecretStoreErrorCode::NotFound =>
                {
                    Ok(None)
                }
                Err(error) => Err(error),
            }
        })?;
        let Some(encoded) = encoded else {
            return Ok(None);
        };
        let session =
            decode_twitch_session(&encoded).map_err(|_| TwitchSessionOwnerError::DecodeFailed)?;
        Ok(Some(state.start(session, false)))
    }
    pub fn begin_new(
        &self,
        session: TwitchSession,
    ) -> Result<TwitchSessionTransaction, TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.idle()?;
        if state.active.is_some() {
            return Err(TwitchSessionOwnerError::Busy);
        }
        Ok(state.start(session, false))
    }
    pub fn checkout_active(&self) -> Result<TwitchSessionTransaction, TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.idle()?;
        let session = state
            .active
            .take()
            .ok_or(TwitchSessionOwnerError::NotConnected)?;
        Ok(state.start(session, true))
    }
    pub fn persist_pending(
        &self,
        id: u64,
        session: &TwitchSession,
    ) -> Result<(), TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.check_identity(id, session)?;
        // A rotation cannot return to the prior generation, even if persistence fails.
        state
            .pending
            .as_mut()
            .expect("checked transaction")
            .restorable = false;
        self.persist(session)
    }
    pub fn commit(
        &self,
        id: u64,
        session: TwitchSession,
    ) -> Result<TwitchSessionOwnerSnapshot, TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.check_identity(id, &session)?;
        self.persist(&session)?;
        state.revision += 1;
        let snapshot = owner_snapshot(&session, state.revision);
        state.active = Some(session);
        state.pending = None;
        Ok(snapshot)
    }
    /// Restores a previously active, unchanged session after a transient pre-rotation failure.
    pub fn restore(&self, id: u64, session: TwitchSession) -> Result<(), TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.check_identity(id, &session)?;
        if !state.check(id)?.restorable {
            return Err(TwitchSessionOwnerError::StaleTransaction);
        }
        encode_twitch_session(&session).map_err(|_| TwitchSessionOwnerError::EncodeFailed)?;
        state.active = Some(session);
        state.pending = None;
        Ok(())
    }
    pub fn discard(&self, id: u64, erase_saved: bool) -> Result<(), TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        state.check(id)?;
        state.active = None;
        state.pending = None;
        if erase_saved {
            self.erase()?;
        }
        Ok(())
    }
    pub fn erase_saved(&self) -> Result<(), TwitchSessionOwnerError> {
        let mut state = self.lock()?;
        if state.stopping {
            return Err(TwitchSessionOwnerError::ShuttingDown);
        }
        if state.pending.is_some() {
            return Err(TwitchSessionOwnerError::Busy);
        }
        state.active = None;
        self.erase()
    }
    pub fn snapshot(&self) -> Option<TwitchSessionOwnerSnapshot> {
        let state = self.lock().ok()?;
        if state.pending.is_some() || state.stopping {
            return None;
        }
        state
            .active
            .as_ref()
            .map(|session| owner_snapshot(session, state.revision))
    }
    pub fn shutdown(&self) {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        state.stopping = true;
        state.active = None;
        state.pending = None;
    }
    pub fn is_shutting_down(&self) -> bool {
        self.lock().map_or(true, |state| state.stopping)
    }
}
impl Drop for TwitchSessionOwner {
    fn drop(&mut self) {
        self.shutdown();
    }
}
fn owner_snapshot(session: &TwitchSession, revision: u64) -> TwitchSessionOwnerSnapshot {
    TwitchSessionOwnerSnapshot {
        authenticated_user_id: session.user_id.clone(),
        login: session.login.clone(),
        access_expires_at: session.credentials.access_expires_at,
        revision,
    }
}
fn lease(session: &TwitchSession, revision: u64) -> TwitchDeliverySession {
    TwitchDeliverySession {
        access_token: SecretValue::from_bytes(session.credentials.access_token.expose().to_vec()),
        authenticated_user_id: session.user_id.clone(),
        revision,
    }
}
fn delivery_error(code: TwitchDeliverySessionErrorCode) -> TwitchDeliverySessionError {
    TwitchDeliverySessionError {
        code,
        retry_after: None,
    }
}
impl From<TwitchSessionOwnerError> for TwitchDeliverySessionError {
    fn from(error: TwitchSessionOwnerError) -> Self {
        delivery_error(match error {
            TwitchSessionOwnerError::Busy | TwitchSessionOwnerError::StaleTransaction => {
                TwitchDeliverySessionErrorCode::Retry
            }
            TwitchSessionOwnerError::NotConnected => TwitchDeliverySessionErrorCode::NotConnected,
            TwitchSessionOwnerError::ShuttingDown => TwitchDeliverySessionErrorCode::Cancelled,
            TwitchSessionOwnerError::IdentityMismatch => {
                TwitchDeliverySessionErrorCode::ReconnectRequired
            }
            _ => TwitchDeliverySessionErrorCode::Failed,
        })
    }
}
impl From<TwitchError> for TwitchDeliverySessionError {
    fn from(error: TwitchError) -> Self {
        Self {
            code: match error.disposition {
                TwitchDisposition::Retry => TwitchDeliverySessionErrorCode::Retry,
                TwitchDisposition::Reconnect => TwitchDeliverySessionErrorCode::ReconnectRequired,
                TwitchDisposition::Failed => TwitchDeliverySessionErrorCode::Failed,
                TwitchDisposition::Cancelled => TwitchDeliverySessionErrorCode::Cancelled,
            },
            retry_after: error
                .retry_after
                .filter(|delay| !delay.is_zero() && *delay <= Duration::from_secs(86400)),
        }
    }
}
