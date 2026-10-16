use crate::{application::*, domain::TwitchDeliveryStatus};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::Arc,
    time::Duration,
};

/// Shared synchronous policy for encounter and explicit test messages. Call only on a worker.
pub struct TwitchChatDelivery {
    client: Arc<dyn TwitchApi>,
    sessions: Arc<dyn TwitchDeliverySessionAccess>,
}
impl TwitchChatDelivery {
    pub fn new(client: Arc<dyn TwitchApi>, sessions: Arc<dyn TwitchDeliverySessionAccess>) -> Self {
        Self { client, sessions }
    }
    pub fn send(&self, message: &str, stop: &StopToken) -> TwitchChatResult {
        if stop.is_stop_requested() {
            return TwitchChatResult::Cancelled;
        }
        if message.is_empty()
            || message.len() > 2000
            || message.chars().count() > 500
            || message.bytes().any(|byte| byte.is_ascii_control())
        {
            return TwitchChatResult::Failed(TwitchChatError::InvalidMessage);
        }
        let mut lease = match catch_unwind(AssertUnwindSafe(|| self.sessions.acquire(stop))) {
            Ok(Ok(lease)) => lease,
            Ok(Err(error)) => return session_failure(error),
            Err(_) => return TwitchChatResult::Failed(TwitchChatError::SessionFailed),
        };
        if !valid_lease(&lease) {
            return TwitchChatResult::Failed(TwitchChatError::SessionFailed);
        }
        let broadcaster = lease.authenticated_user_id.clone();
        let revision = lease.revision;
        for attempt in 0..2 {
            if stop.is_stop_requested() {
                return TwitchChatResult::Cancelled;
            }
            let sent = catch_unwind(AssertUnwindSafe(|| {
                self.client.send_chat_message(
                    &lease.authenticated_user_id,
                    message,
                    &lease.access_token,
                    stop,
                )
            }));
            // A cancelled or panicking call may already have delivered the message.
            if stop.is_stop_requested() {
                return TwitchChatResult::Ambiguous;
            }
            let sent = match sent {
                Ok(sent) => sent,
                Err(_) => return TwitchChatResult::Ambiguous,
            };
            match sent {
                Ok(receipt) => {
                    if !receipt.is_valid() {
                        return TwitchChatResult::Ambiguous;
                    }
                    return if receipt.status == TwitchDeliveryStatus::Sent {
                        TwitchChatResult::Sent(receipt)
                    } else {
                        TwitchChatResult::Dropped(receipt)
                    };
                }
                Err(error)
                    if error.code == TwitchErrorCode::HttpStatus
                        && error.http_status == Some(401) =>
                {
                    if attempt == 1 {
                        return TwitchChatResult::Failed(TwitchChatError::ReconnectRequired);
                    }
                    lease =
                        match catch_unwind(AssertUnwindSafe(|| self.sessions.recover(lease, stop)))
                        {
                            Ok(Ok(recovered)) => recovered,
                            Ok(Err(error)) => return session_failure(error),
                            Err(_) => {
                                return TwitchChatResult::Failed(TwitchChatError::SessionFailed);
                            }
                        };
                    if !valid_lease(&lease)
                        || lease.authenticated_user_id != broadcaster
                        || lease.revision < revision
                    {
                        return TwitchChatResult::Failed(TwitchChatError::ReconnectRequired);
                    }
                }
                Err(error) => return send_failure(error),
            }
        }
        unreachable!("second send always settles")
    }
}
fn valid_lease(lease: &TwitchDeliverySession) -> bool {
    lease.revision != 0
        && valid_twitch_user_id(&lease.authenticated_user_id)
        && valid_twitch_token(&lease.access_token)
}
pub(super) fn retry_delay(delay: Option<Duration>) -> Duration {
    delay
        .filter(|delay| !delay.is_zero() && *delay <= Duration::from_secs(86400))
        .unwrap_or(Duration::from_secs(30))
}
fn session_failure(error: TwitchDeliverySessionError) -> TwitchChatResult {
    match error.code {
        TwitchDeliverySessionErrorCode::Retry => {
            TwitchChatResult::RetryAfter(retry_delay(error.retry_after))
        }
        TwitchDeliverySessionErrorCode::Cancelled => TwitchChatResult::Cancelled,
        TwitchDeliverySessionErrorCode::NotConnected => {
            TwitchChatResult::Failed(TwitchChatError::NotConnected)
        }
        TwitchDeliverySessionErrorCode::ReconnectRequired => {
            TwitchChatResult::Failed(TwitchChatError::ReconnectRequired)
        }
        TwitchDeliverySessionErrorCode::Failed => {
            TwitchChatResult::Failed(TwitchChatError::SessionFailed)
        }
    }
}
fn send_failure(error: TwitchError) -> TwitchChatResult {
    let safe_connection = error.code == TwitchErrorCode::Transport
        && matches!(
            error.http_error,
            Some(
                HttpErrorCode::NameResolutionFailed
                    | HttpErrorCode::ConnectionFailed
                    | HttpErrorCode::TlsFailed
            )
        );
    let rate_limited = error.code == TwitchErrorCode::HttpStatus && error.http_status == Some(429);
    if error.disposition == TwitchDisposition::Retry && (safe_connection || rate_limited) {
        return TwitchChatResult::RetryAfter(retry_delay(error.retry_after));
    }
    if error.code == TwitchErrorCode::HttpStatus
        && error
            .http_status
            .is_some_and(|status| (400..500).contains(&status) && status != 408)
    {
        return TwitchChatResult::Failed(TwitchChatError::SendFailed);
    }
    TwitchChatResult::Ambiguous
}
