use super::{TwitchChatError, TwitchConnectionSnapshot, TwitchConnectionState, twitch_drop_detail};
use crate::domain::TwitchDeliveryStatus;
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    time::Duration,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TwitchTestMessageRequest {
    pub request_id: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchTestMessageOutcome {
    Sent,
    Dropped(TwitchDeliveryStatus),
    RetryAfter(Duration),
    Failed(TwitchChatError),
    Ambiguous,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TwitchTestMessageResult {
    pub request_id: u64,
    pub outcome: TwitchTestMessageOutcome,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchTestMessageDispatchError {
    InvalidRequest,
    QueueFull,
    Stopping,
}
pub trait TwitchTestMessenger: Send {
    fn enqueue(
        &mut self,
        request: TwitchTestMessageRequest,
    ) -> Result<(), TwitchTestMessageDispatchError>;
    fn try_take_result(&mut self) -> Option<TwitchTestMessageResult>;
    fn cancel_pending(&mut self);
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchTestMessageState {
    Idle,
    Sending,
    Sent,
    Error,
    ShuttingDown,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchTestMessageError {
    Busy,
    NotConnected,
    DispatchFailed,
    StaleResult,
    InvalidResult,
    RetrySuppressed,
    Failed(TwitchChatError),
    Dropped(TwitchDeliveryStatus),
    Ambiguous,
    Cancelled,
    ShuttingDown,
    RevisionExhausted,
}
impl TwitchTestMessageError {
    pub fn detail(self) -> &'static str {
        match self {
            Self::Busy => "A Twitch test message is already in progress",
            Self::NotConnected => "Connect Twitch before sending a test message",
            Self::DispatchFailed => "The Twitch test message could not be started",
            Self::StaleResult => "A stale Twitch test result was ignored",
            Self::InvalidResult => "The Twitch test result was invalid",
            Self::RetrySuppressed => {
                "Twitch is temporarily unavailable. Send another test when ready"
            }
            Self::Failed(error) => error.detail(),
            Self::Dropped(status) => twitch_drop_detail(status),
            Self::Ambiguous => {
                "Twitch delivery could not be confirmed. Automatic retry is disabled"
            }
            Self::Cancelled => "The Twitch test message was cancelled",
            Self::ShuttingDown => "Twitch test messages are shutting down",
            Self::RevisionExhausted => "Twitch cannot accept more test messages",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchTestMessageSnapshot {
    pub state: TwitchTestMessageState,
    pub request_id: Option<u64>,
    pub delivery_status: Option<TwitchDeliveryStatus>,
    pub diagnostic: Option<TwitchTestMessageError>,
    pub revision: u64,
}
pub struct TwitchTestMessageController {
    messenger: Box<dyn TwitchTestMessenger>,
    snapshot: TwitchTestMessageSnapshot,
    pending: Option<u64>,
    next_request: u64,
}
impl TwitchTestMessageController {
    pub fn new(messenger: Box<dyn TwitchTestMessenger>) -> Self {
        Self {
            messenger,
            snapshot: TwitchTestMessageSnapshot {
                state: TwitchTestMessageState::Idle,
                request_id: None,
                delivery_status: None,
                diagnostic: None,
                revision: 1,
            },
            pending: None,
            next_request: 1,
        }
    }
    pub fn snapshot(&self) -> TwitchTestMessageSnapshot {
        self.snapshot.clone()
    }
    fn running(&self) -> Result<(), TwitchTestMessageError> {
        if self.snapshot.state == TwitchTestMessageState::ShuttingDown {
            return Err(TwitchTestMessageError::ShuttingDown);
        }
        if self.next_request == u64::MAX || self.snapshot.revision >= u64::MAX - 1 {
            return Err(TwitchTestMessageError::RevisionExhausted);
        }
        Ok(())
    }
    pub fn send(
        &mut self,
        connection: &TwitchConnectionSnapshot,
    ) -> Result<u64, TwitchTestMessageError> {
        self.running()?;
        if self.pending.is_some() {
            return Err(TwitchTestMessageError::Busy);
        }
        if connection.shutting_down || connection.state != TwitchConnectionState::Connected {
            return Err(TwitchTestMessageError::NotConnected);
        }
        let id = self.next_request;
        self.next_request += 1;
        self.snapshot.request_id = Some(id);
        self.snapshot.delivery_status = None;
        match catch_unwind(AssertUnwindSafe(|| {
            self.messenger
                .enqueue(TwitchTestMessageRequest { request_id: id })
        })) {
            Ok(Ok(())) => {
                self.pending = Some(id);
                self.publish(TwitchTestMessageState::Sending, None);
                Ok(id)
            }
            Ok(Err(_)) => self.fail(TwitchTestMessageError::DispatchFailed),
            Err(_) => self.fail(TwitchTestMessageError::Ambiguous),
        }
    }
    pub fn poll(&mut self) -> Result<bool, TwitchTestMessageError> {
        self.running()?;
        if self.pending.is_none() {
            return Ok(false);
        }
        let result = match catch_unwind(AssertUnwindSafe(|| self.messenger.try_take_result())) {
            Ok(Some(result)) => result,
            Ok(None) => return Ok(false),
            Err(_) => return self.fail(TwitchTestMessageError::Ambiguous),
        };
        if self.pending != Some(result.request_id) {
            return Err(TwitchTestMessageError::StaleResult);
        }
        self.pending = None;
        match result.outcome {
            TwitchTestMessageOutcome::Sent => {
                self.snapshot.delivery_status = Some(TwitchDeliveryStatus::Sent);
                self.publish(TwitchTestMessageState::Sent, None);
            }
            TwitchTestMessageOutcome::Dropped(TwitchDeliveryStatus::Sent) => {
                return self.fail(TwitchTestMessageError::InvalidResult);
            }
            TwitchTestMessageOutcome::Dropped(status) => {
                self.snapshot.delivery_status = Some(status);
                return self.fail(TwitchTestMessageError::Dropped(status));
            }
            TwitchTestMessageOutcome::RetryAfter(_) => {
                return self.fail(TwitchTestMessageError::RetrySuppressed);
            }
            TwitchTestMessageOutcome::Failed(error) => {
                return self.fail(TwitchTestMessageError::Failed(error));
            }
            TwitchTestMessageOutcome::Ambiguous => {
                return self.fail(TwitchTestMessageError::Ambiguous);
            }
            TwitchTestMessageOutcome::Cancelled => {
                return self.fail(TwitchTestMessageError::Cancelled);
            }
        }
        Ok(true)
    }
    fn fail<T>(&mut self, error: TwitchTestMessageError) -> Result<T, TwitchTestMessageError> {
        self.pending = None;
        self.publish(TwitchTestMessageState::Error, Some(error));
        Err(error)
    }
    fn publish(
        &mut self,
        state: TwitchTestMessageState,
        diagnostic: Option<TwitchTestMessageError>,
    ) {
        self.snapshot.state = state;
        self.snapshot.diagnostic = diagnostic;
        self.snapshot.revision += 1;
    }
    pub fn shutdown(&mut self) {
        if self.snapshot.state == TwitchTestMessageState::ShuttingDown {
            return;
        }
        self.pending = None;
        self.snapshot.request_id = None;
        self.snapshot.delivery_status = None;
        self.publish(TwitchTestMessageState::ShuttingDown, None);
        let _ = catch_unwind(AssertUnwindSafe(|| self.messenger.cancel_pending()));
    }
}
impl Drop for TwitchTestMessageController {
    fn drop(&mut self) {
        self.shutdown();
    }
}
