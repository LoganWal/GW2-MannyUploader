use super::{UploadFailure, UploadOutcome, UploadSuccess};
use crate::domain::{TwitchDeliveryReceipt, TwitchDeliveryStatus};
use std::time::Duration;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchChatError {
    InvalidMessage,
    NotConnected,
    ReconnectRequired,
    SessionFailed,
    SendFailed,
}
impl TwitchChatError {
    pub const fn detail(self) -> &'static str {
        match self {
            Self::InvalidMessage => "The Twitch chat message is invalid",
            Self::NotConnected => "Connect Twitch before posting to chat",
            Self::ReconnectRequired => "Reconnect Twitch before posting again",
            Self::SessionFailed => "The Twitch session is unavailable",
            Self::SendFailed => "Twitch rejected the chat request",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TwitchChatResult {
    Sent(TwitchDeliveryReceipt),
    Dropped(TwitchDeliveryReceipt),
    RetryAfter(Duration),
    Failed(TwitchChatError),
    Ambiguous,
    Cancelled,
}
impl TwitchChatResult {
    pub fn into_upload_outcome(self) -> UploadOutcome {
        match self {
            Self::Sent(receipt) => UploadOutcome::Succeeded(UploadSuccess::Twitch(receipt)),
            Self::Dropped(receipt) => UploadOutcome::TwitchDropped(receipt),
            Self::RetryAfter(delay) => UploadOutcome::RetryAfter(delay),
            Self::Failed(error) => UploadOutcome::FailedWithNotice(UploadFailure::Twitch(error)),
            Self::Ambiguous => UploadOutcome::Ambiguous,
            Self::Cancelled => UploadOutcome::Cancelled,
        }
    }
    pub fn detail(&self) -> &'static str {
        match self {
            Self::Sent(_) => "Posted to Twitch chat",
            Self::Dropped(receipt) => twitch_drop_detail(receipt.status),
            Self::RetryAfter(_) => "Twitch chat is temporarily unavailable",
            Self::Failed(error) => error.detail(),
            Self::Ambiguous => {
                "Twitch delivery could not be confirmed. Automatic retry is disabled"
            }
            Self::Cancelled => "Twitch chat delivery was cancelled",
        }
    }
}
pub fn twitch_drop_detail(status: TwitchDeliveryStatus) -> &'static str {
    match status {
        TwitchDeliveryStatus::Sent => "Posted to Twitch chat",
        TwitchDeliveryStatus::AutoMod => "Twitch held the message for moderation",
        TwitchDeliveryStatus::BlockedTerm => "Twitch rejected a blocked term",
        TwitchDeliveryStatus::Duplicate => "Twitch rejected a duplicate message",
        TwitchDeliveryStatus::RateLimited => "Twitch chat is rate limited",
        TwitchDeliveryStatus::FollowersOnly => "Twitch chat requires a follower",
        TwitchDeliveryStatus::SlowMode => "Twitch chat slow mode is active",
        TwitchDeliveryStatus::SubscribersOnly => "Twitch chat requires a subscriber",
        TwitchDeliveryStatus::Restricted => "The Twitch account or channel is restricted",
        TwitchDeliveryStatus::OtherDrop => "Twitch rejected the chat message",
    }
}
