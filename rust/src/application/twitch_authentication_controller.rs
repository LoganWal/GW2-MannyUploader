mod results;

use super::*;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex},
    time::{Duration, Instant, SystemTime},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchConnectionState {
    Disconnected,
    Starting,
    AwaitingUser,
    Validating,
    Refreshing,
    Connected,
    Disconnecting,
    Error,
    ShuttingDown,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchAuthenticationControllerError {
    Busy,
    InvalidState,
    DispatchFailed,
    StaleResult,
    InvalidResult,
    AuthorizationExpired,
    AuthenticationFailed,
    AuthenticationCancelled,
    ReconnectRequired,
    SessionLoadFailed,
    SessionDecodeFailed,
    SessionStoreFailed,
    SessionEraseFailed,
    SettingsSaveFailed,
    IdentityMismatch,
    ShuttingDown,
    RevisionExhausted,
    Internal,
}
type Error = TwitchAuthenticationControllerError;
type State = TwitchConnectionState;
impl Error {
    pub fn detail(self) -> &'static str {
        match self {
            Self::Busy => "Twitch authentication is already in progress",
            Self::InvalidState => "This Twitch action is unavailable",
            Self::DispatchFailed => "Twitch authentication could not be started",
            Self::StaleResult => "A stale Twitch authentication result was ignored",
            Self::InvalidResult => "Twitch authentication returned an invalid result",
            Self::AuthorizationExpired => "Twitch authorization expired. Connect again",
            Self::AuthenticationFailed => "Twitch authentication failed",
            Self::AuthenticationCancelled => "Twitch authentication was cancelled",
            Self::ReconnectRequired => "Reconnect Twitch to continue",
            Self::SessionLoadFailed => "The saved Twitch session could not be loaded",
            Self::SessionDecodeFailed => "The saved Twitch session is invalid",
            Self::SessionStoreFailed => "The Twitch session could not be saved",
            Self::SessionEraseFailed => "The saved Twitch session could not be removed",
            Self::SettingsSaveFailed => "Twitch settings could not be saved",
            Self::IdentityMismatch => "Twitch authentication changed broadcaster identity",
            Self::ShuttingDown => "Twitch authentication is shutting down",
            Self::RevisionExhausted => "Twitch authentication cannot accept more changes",
            Self::Internal => "Twitch authentication could not be completed",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchConnectionSnapshot {
    pub state: TwitchConnectionState,
    pub login: Option<String>,
    pub user_code: Option<String>,
    pub verification_uri: Option<String>,
    pub authorization_expires_at: Option<SystemTime>,
    pub access_expires_at: Option<SystemTime>,
    pub diagnostic: Option<TwitchAuthenticationControllerError>,
    pub revision: u64,
    pub shutting_down: bool,
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Start,
    Poll,
    ValidateInitial,
    ValidateSaved,
    ValidateActive,
    ValidateRotated,
    Refresh,
    Revoke,
}
impl Phase {
    fn operation(self) -> TwitchOperation {
        match self {
            Self::Start => TwitchOperation::Start,
            Self::Poll => TwitchOperation::Poll,
            Self::Refresh => TwitchOperation::Refresh,
            Self::Revoke => TwitchOperation::Revoke,
            _ => TwitchOperation::Validate,
        }
    }
    fn state(self) -> State {
        match self {
            Self::Start => State::Starting,
            Self::Poll => State::AwaitingUser,
            Self::Refresh => State::Refreshing,
            Self::Revoke => State::Disconnecting,
            _ => State::Validating,
        }
    }
}
struct InFlight {
    id: u64,
    phase: Phase,
}
struct Authorization {
    code: Option<SecretValue>,
    expires: Instant,
    next_poll: Instant,
    interval: Duration,
}
struct Draft {
    credentials: Option<TwitchCredentialSet>,
    identity: Option<(String, String)>,
    rotated: bool,
}
impl Draft {
    fn from_session(session: TwitchSession) -> Self {
        Self {
            credentials: Some(session.credentials),
            identity: Some((session.user_id, session.login)),
            rotated: false,
        }
    }
}
struct Schedule {
    owner_revision: u64,
    validation: Instant,
    refresh: Instant,
}

/// Application-thread workflow. The worker owns network calls, and the shared owner serializes
/// protected sessions with chat recovery. Render callbacks receive copies of the public snapshot.
pub struct TwitchAuthenticationController {
    configuration: Arc<Mutex<ConfigurationService>>,
    authenticator: Box<dyn TwitchAuthenticator>,
    owner: Arc<TwitchSessionOwner>,
    clock: Arc<dyn Clock + Send + Sync>,
    snapshot: TwitchConnectionSnapshot,
    transaction: Option<u64>,
    draft: Option<Draft>,
    authorization: Option<Authorization>,
    pending: Option<InFlight>,
    deferred: Option<(Phase, Instant)>,
    schedule: Option<Schedule>,
    next_request: u64,
}
impl TwitchAuthenticationController {
    pub fn new(
        configuration: Arc<Mutex<ConfigurationService>>,
        authenticator: Box<dyn TwitchAuthenticator>,
        owner: Arc<TwitchSessionOwner>,
        clock: Arc<dyn Clock + Send + Sync>,
    ) -> Result<Self, Error> {
        if configuration
            .lock()
            .map_err(|_| Error::Internal)?
            .is_shutting_down()
            || owner.is_shutting_down()
        {
            return Err(Error::ShuttingDown);
        }
        Ok(Self {
            configuration,
            authenticator,
            owner,
            clock,
            snapshot: TwitchConnectionSnapshot {
                state: State::Disconnected,
                login: None,
                user_code: None,
                verification_uri: None,
                authorization_expires_at: None,
                access_expires_at: None,
                diagnostic: None,
                revision: 1,
                shutting_down: false,
            },
            transaction: None,
            draft: None,
            authorization: None,
            pending: None,
            deferred: None,
            schedule: None,
            next_request: 1,
        })
    }
    pub fn snapshot(&self) -> TwitchConnectionSnapshot {
        self.snapshot.clone()
    }
    pub fn is_shutting_down(&self) -> bool {
        self.snapshot.shutting_down
    }
    fn running(&self) -> Result<(), Error> {
        if self.snapshot.shutting_down
            || self.owner.is_shutting_down()
            || self
                .configuration
                .lock()
                .map_err(|_| Error::Internal)?
                .is_shutting_down()
        {
            return Err(Error::ShuttingDown);
        }
        if self.snapshot.revision >= u64::MAX - 1 || self.next_request == u64::MAX {
            return Err(Error::RevisionExhausted);
        }
        Ok(())
    }
    fn idle_connection(&self) -> Result<(), Error> {
        self.running()?;
        if self.pending.is_some() || self.transaction.is_some() {
            return Err(Error::Busy);
        }
        if !matches!(self.snapshot.state, State::Disconnected | State::Error) {
            return Err(Error::InvalidState);
        }
        Ok(())
    }
    pub fn begin_connection(&mut self) -> Result<(), Error> {
        self.idle_connection()?;
        self.transaction = Some(self.owner.reserve_new().map_err(owner_error)?);
        self.clear_public();
        self.dispatch(Phase::Start, TwitchAuthenticationCommand::Start)
    }
    pub fn begin_saved_connection(&mut self) -> Result<bool, Error> {
        self.idle_connection()?;
        let tx = match self.owner.checkout_saved() {
            Ok(Some(tx)) => tx,
            Ok(None) => return Ok(false),
            Err(error) => return self.fail(owner_error(error), false),
        };
        self.adopt(tx);
        self.clear_public();
        self.dispatch_credentials(Phase::ValidateSaved)?;
        Ok(true)
    }
    fn adopt(&mut self, tx: TwitchSessionTransaction) {
        self.transaction = Some(tx.id);
        self.draft = Some(Draft::from_session(tx.session));
    }
    fn dispatch_credentials(&mut self, phase: Phase) -> Result<(), Error> {
        let Some(credentials) = self
            .draft
            .as_mut()
            .and_then(|draft| draft.credentials.take())
        else {
            return self.fail(Error::InvalidState, false);
        };
        let command = if phase == Phase::Refresh {
            TwitchAuthenticationCommand::Refresh { credentials }
        } else {
            TwitchAuthenticationCommand::Validate { credentials }
        };
        self.dispatch(phase, command)
    }
    fn dispatch(
        &mut self,
        phase: Phase,
        command: TwitchAuthenticationCommand,
    ) -> Result<(), Error> {
        if self.pending.is_some() || self.next_request == u64::MAX {
            return self.fail(Error::RevisionExhausted, false);
        }
        let id = self.next_request;
        self.next_request += 1;
        self.pending = Some(InFlight { id, phase });
        let queued = catch_unwind(AssertUnwindSafe(|| {
            self.authenticator.enqueue(TwitchAuthenticationRequest {
                request_id: id,
                command,
            })
        }));
        if !matches!(queued, Ok(Ok(()))) {
            if phase == Phase::Revoke {
                return self.finish_disconnect(Some(Error::DispatchFailed));
            }
            return self.fail(
                Error::DispatchFailed,
                self.draft.as_ref().is_some_and(|draft| draft.rotated),
            );
        }
        self.snapshot.state = phase.state();
        self.snapshot.diagnostic = None;
        self.bump();
        Ok(())
    }
    pub fn tick(&mut self) -> Result<bool, Error> {
        self.running()?;
        catch_unwind(AssertUnwindSafe(|| self.tick_inner())).unwrap_or_else(|_| {
            if self.snapshot.state == State::Disconnecting {
                self.finish_disconnect(Some(Error::Internal))?;
                Ok(true)
            } else {
                self.fail(
                    Error::Internal,
                    self.draft.as_ref().is_some_and(|draft| draft.rotated),
                )
            }
        })
    }
    fn tick_inner(&mut self) -> Result<bool, Error> {
        let now = self.clock.steady_now();
        // An in-flight poll can finish after expiry. Drain it without accepting a late grant.
        if self
            .authorization
            .as_ref()
            .is_some_and(|authorization| now >= authorization.expires)
        {
            return self.fail(Error::AuthorizationExpired, false);
        }
        if let Some(result) = self.authenticator.try_take_result() {
            return self.handle_result(result);
        }
        if self.pending.is_some() {
            return Ok(false);
        }
        if let Some((phase, deadline)) = self.deferred {
            if now < deadline {
                return Ok(false);
            }
            self.deferred = None;
            self.dispatch_credentials(phase)?;
            return Ok(true);
        }
        if let Some(authorization) = &mut self.authorization {
            if now < authorization.next_poll {
                return Ok(false);
            }
            let Some(device_code) = authorization.code.take() else {
                return self.fail(Error::InvalidState, false);
            };
            self.dispatch(
                Phase::Poll,
                TwitchAuthenticationCommand::Poll { device_code },
            )?;
            return Ok(true);
        }
        if self.schedule.is_none()
            || !matches!(self.snapshot.state, State::Connected | State::Validating)
        {
            return Ok(false);
        }
        let active = match self.owner.connection_state().map_err(owner_error)? {
            TwitchSessionOwnerState::Disconnected => {
                return self.fail(Error::ReconnectRequired, false);
            }
            TwitchSessionOwnerState::ShuttingDown => return self.fail(Error::ShuttingDown, false),
            TwitchSessionOwnerState::Connected(active) => active,
            TwitchSessionOwnerState::Updating => {
                // A chat recovery may temporarily own the transaction. Hide identity until it settles.
                if self.snapshot.state == State::Validating {
                    return Ok(false);
                }
                self.snapshot.state = State::Validating;
                self.snapshot.login = None;
                self.snapshot.access_expires_at = None;
                self.bump();
                return Ok(true);
            }
        };
        if self.snapshot.state != State::Connected
            && self
                .schedule
                .as_ref()
                .is_some_and(|schedule| schedule.owner_revision == active.revision)
        {
            // A transient recovery did not validate a newer generation. Keep the original deadlines.
            let schedule = self.schedule.take().expect("existing schedule");
            self.set_connected(active, schedule);
            return Ok(true);
        }
        if self.snapshot.state != State::Connected
            || self
                .schedule
                .as_ref()
                .is_none_or(|schedule| schedule.owner_revision != active.revision)
        {
            self.publish_connected(active)?;
            return Ok(true);
        }
        let schedule = self.schedule.as_ref().expect("connected schedule");
        let phase = if now >= schedule.refresh {
            Phase::Refresh
        } else if now >= schedule.validation {
            Phase::ValidateActive
        } else {
            return Ok(false);
        };
        match self.owner.checkout_active() {
            Ok(tx) => self.adopt(tx),
            Err(TwitchSessionOwnerError::Busy) => return Ok(false),
            Err(error) => return self.fail(owner_error(error), false),
        }
        self.clear_public();
        self.dispatch_credentials(phase)?;
        Ok(true)
    }
    pub fn disconnect(&mut self) -> Result<(), Error> {
        self.running()?;
        if self.pending.is_some() {
            return Err(Error::Busy);
        }
        self.disable_posting()?;
        self.authorization = None;
        self.deferred = None;
        if self.transaction.is_none() {
            let tx = match self.owner.checkout_active() {
                Ok(tx) => Some(tx),
                Err(TwitchSessionOwnerError::NotConnected) => match self.owner.checkout_saved() {
                    Ok(tx) => tx,
                    Err(TwitchSessionOwnerError::DecodeFailed) => None,
                    Err(error) => return Err(owner_error(error)),
                },
                Err(error) => return Err(owner_error(error)),
            };
            if let Some(tx) = tx {
                self.adopt(tx);
            }
        }
        self.clear_public();
        if let Some(credentials) = self
            .draft
            .as_mut()
            .and_then(|draft| draft.credentials.take())
        {
            return self.dispatch(
                Phase::Revoke,
                TwitchAuthenticationCommand::Revoke {
                    access_token: credentials.access_token,
                },
            );
        }
        self.finish_disconnect(None)
    }
    fn disable_posting(&self) -> Result<(), Error> {
        let mut configuration = self
            .configuration
            .lock()
            .map_err(|_| Error::SettingsSaveFailed)?;
        let mut settings = configuration.snapshot().settings;
        settings.twitch.enabled = false;
        catch_unwind(AssertUnwindSafe(|| configuration.save_settings(settings)))
            .map_err(|_| Error::SettingsSaveFailed)?
            .map_err(|_| Error::SettingsSaveFailed)
    }
    fn finish_disconnect(&mut self, diagnostic: Option<Error>) -> Result<(), Error> {
        let erased = match self.transaction.take() {
            Some(id) => self.owner.discard(id, true),
            None => self.owner.erase_saved(),
        };
        self.reset();
        self.clear_public();
        let diagnostic = erased.err().map(owner_error).or(diagnostic);
        self.snapshot.state = if matches!(diagnostic, Some(Error::SessionEraseFailed)) {
            State::Error
        } else {
            State::Disconnected
        };
        self.snapshot.diagnostic = diagnostic;
        self.bump();
        diagnostic.map_or(Ok(()), Err)
    }
    fn publish_connected(&mut self, connected: TwitchSessionOwnerSnapshot) -> Result<(), Error> {
        let schedule = match self.make_schedule(connected.access_expires_at) {
            Ok(schedule) => schedule,
            Err(error) => return self.fail(error, false),
        };
        self.set_connected(connected, schedule);
        Ok(())
    }
    fn make_schedule(&self, expiry: SystemTime) -> Result<Schedule, Error> {
        let now = self.clock.steady_now();
        let remaining = expiry
            .duration_since(self.clock.system_now())
            .unwrap_or_default();
        Ok(Schedule {
            owner_revision: 0,
            validation: now
                .checked_add(Duration::from_secs(3600))
                .ok_or(Error::InvalidResult)?,
            refresh: now
                .checked_add(remaining.saturating_sub(Duration::from_secs(300)))
                .ok_or(Error::InvalidResult)?,
        })
    }
    fn set_connected(&mut self, connected: TwitchSessionOwnerSnapshot, mut schedule: Schedule) {
        schedule.owner_revision = connected.revision;
        self.schedule = Some(schedule);
        self.draft = None;
        self.authorization = None;
        self.deferred = None;
        self.clear_public();
        self.snapshot.state = State::Connected;
        self.snapshot.login = Some(connected.login);
        self.snapshot.access_expires_at = Some(connected.access_expires_at);
        self.snapshot.diagnostic = None;
        self.bump();
    }
    fn fail<T>(&mut self, mut error: Error, erase: bool) -> Result<T, Error> {
        if let Some(id) = self.transaction.take() {
            if let Err(cleanup) = self.owner.discard(id, erase) {
                if erase {
                    error = owner_error(cleanup);
                }
            }
        }
        self.reset();
        self.clear_public();
        self.snapshot.state = State::Error;
        self.snapshot.diagnostic = Some(error);
        self.bump();
        Err(error)
    }
    fn reset(&mut self) {
        self.draft = None;
        self.authorization = None;
        self.pending = None;
        self.deferred = None;
        self.schedule = None;
    }
    fn clear_public(&mut self) {
        self.snapshot.login = None;
        self.snapshot.user_code = None;
        self.snapshot.verification_uri = None;
        self.snapshot.authorization_expires_at = None;
        self.snapshot.access_expires_at = None;
    }
    fn bump(&mut self) {
        self.snapshot.revision = self.snapshot.revision.saturating_add(1);
    }
    pub fn shutdown(&mut self) {
        if self.snapshot.shutting_down {
            return;
        }
        let disconnect_error = if self.snapshot.state == State::Disconnecting {
            self.finish_disconnect(None).err()
        } else {
            None
        };
        // Invalidate transactions before joining the worker so delayed results cannot publish.
        self.owner.shutdown();
        self.transaction = None;
        self.reset();
        self.clear_public();
        self.snapshot.state = State::ShuttingDown;
        self.snapshot.shutting_down = true;
        self.snapshot.diagnostic = disconnect_error;
        self.bump();
        let _ = catch_unwind(AssertUnwindSafe(|| self.authenticator.cancel_pending()));
    }
}
impl Drop for TwitchAuthenticationController {
    fn drop(&mut self) {
        self.shutdown();
    }
}
fn owner_error(error: TwitchSessionOwnerError) -> Error {
    match error {
        TwitchSessionOwnerError::Busy => Error::Busy,
        TwitchSessionOwnerError::NotConnected => Error::InvalidState,
        TwitchSessionOwnerError::LoadFailed => Error::SessionLoadFailed,
        TwitchSessionOwnerError::DecodeFailed => Error::SessionDecodeFailed,
        TwitchSessionOwnerError::EncodeFailed | TwitchSessionOwnerError::StoreFailed => {
            Error::SessionStoreFailed
        }
        TwitchSessionOwnerError::EraseFailed => Error::SessionEraseFailed,
        TwitchSessionOwnerError::IdentityMismatch => Error::IdentityMismatch,
        TwitchSessionOwnerError::ShuttingDown => Error::ShuttingDown,
        TwitchSessionOwnerError::RevisionExhausted => Error::RevisionExhausted,
        TwitchSessionOwnerError::StaleTransaction | TwitchSessionOwnerError::Internal => {
            Error::Internal
        }
    }
}
