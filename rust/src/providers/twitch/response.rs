use serde::{Deserialize, Deserializer, de::Visitor};
use std::{fmt, time::Duration};

use super::*;
use crate::{
    domain::{TwitchDeliveryReceipt, TwitchDeliveryStatus},
    providers::json::{self, List, Object, Text},
};

fn invalid(op: TwitchOperation) -> TwitchError {
    TwitchError {
        disposition: if op == TwitchOperation::Validate {
            TwitchDisposition::Reconnect
        } else {
            TwitchDisposition::Failed
        },
        http_status: Some(200),
        ..TwitchError::failed(op, TwitchErrorCode::InvalidResponse)
    }
}
fn positive_duration(seconds: u64) -> Option<Duration> {
    (1..=604_800)
        .contains(&seconds)
        .then_some(Duration::from_secs(seconds))
}
fn visible(text: &str) -> bool {
    !text.is_empty() && !text.bytes().any(|b| b.is_ascii_control())
}
fn activation_uri(uri: &str) -> bool {
    uri.strip_prefix("https://www.twitch.tv/activate")
        .is_some_and(|rest| rest.is_empty() || rest.starts_with('?'))
        && !uri.contains(['@', '#', '\\'])
        && uri.bytes().all(|b| (0x21..=0x7e).contains(&b))
}

struct Token(SecretValue);
impl<'de> Deserialize<'de> for Token {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct TokenVisitor;
        impl Visitor<'_> for TokenVisitor {
            type Value = Token;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a bounded token")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Self::Value, E> {
                if value.is_empty()
                    || value.len() > MAX_TWITCH_TOKEN_BYTES
                    || !value.bytes().all(|b| (0x21..=0x7e).contains(&b))
                {
                    return Err(E::custom("invalid token"));
                }
                Ok(Token(SecretValue::from_bytes(value.as_bytes().to_vec())))
            }
        }
        deserializer.deserialize_str(TokenVisitor)
    }
}
#[derive(Deserialize)]
struct Device {
    device_code: Token,
    user_code: Text<64>,
    verification_uri: Text<2048>,
    expires_in: u64,
    interval: u64,
}
pub(super) fn device(response: &HttpResponse) -> Result<TwitchDeviceAuthorization, TwitchError> {
    let op = TwitchOperation::Start;
    let parsed: Device =
        json::decode(response.body.expose(), MAX_RESPONSE_BYTES).map_err(|_| invalid(op))?;
    if !visible(&parsed.user_code.0)
        || !activation_uri(&parsed.verification_uri.0)
        || !(1..=300).contains(&parsed.interval)
    {
        return Err(invalid(op));
    }
    Ok(TwitchDeviceAuthorization {
        device_code: parsed.device_code.0,
        user_code: parsed.user_code.0,
        verification_uri: parsed.verification_uri.0,
        expires_in: positive_duration(parsed.expires_in).ok_or_else(|| invalid(op))?,
        polling_interval: Duration::from_secs(parsed.interval),
    })
}
#[derive(Deserialize)]
struct Pending {
    status: u16,
    message: Text<128>,
}
pub(super) fn pending(response: &HttpResponse) -> bool {
    json::decode::<Pending>(response.body.expose(), MAX_RESPONSE_BYTES)
        .is_ok_and(|pending| pending.status == 400 && pending.message.0 == "authorization_pending")
}
#[derive(Deserialize)]
#[serde(untagged)]
enum Scope {
    One(Text<128>),
    Many(List<Text<128>, 1>),
}
impl Scope {
    fn valid(&self) -> bool {
        match self {
            Self::One(text) => text.0 == TWITCH_CHAT_SCOPE,
            Self::Many(list) => list.0.len() == 1 && list.0[0].0 == TWITCH_CHAT_SCOPE,
        }
    }
}
#[derive(Deserialize)]
struct Grant {
    access_token: Token,
    refresh_token: Token,
    expires_in: u64,
    scope: Scope,
    token_type: Text<16>,
}
pub(super) fn grant(
    response: &HttpResponse,
    op: TwitchOperation,
) -> Result<TwitchTokenGrant, TwitchError> {
    let parsed: Grant =
        json::decode(response.body.expose(), MAX_RESPONSE_BYTES).map_err(|_| invalid(op))?;
    if !parsed.scope.valid() || !parsed.token_type.0.eq_ignore_ascii_case("bearer") {
        return Err(invalid(op));
    }
    Ok(TwitchTokenGrant {
        access_token: parsed.access_token.0,
        refresh_token: parsed.refresh_token.0,
        expires_in: positive_duration(parsed.expires_in).ok_or_else(|| invalid(op))?,
        scopes: vec![TWITCH_CHAT_SCOPE.into()],
    })
}
#[derive(Deserialize)]
struct Identity {
    client_id: Text<128>,
    user_id: Text<20>,
    login: Text<64>,
    scopes: List<Text<128>, 1>,
    expires_in: u64,
}
pub(super) fn identity(
    response: &HttpResponse,
    client_id: &str,
) -> Result<TwitchValidatedIdentity, TwitchError> {
    let op = TwitchOperation::Validate;
    let parsed: Identity =
        json::decode(response.body.expose(), MAX_RESPONSE_BYTES).map_err(|_| invalid(op))?;
    if parsed.client_id.0 != client_id
        || !valid_twitch_user_id(&parsed.user_id.0)
        || !valid_twitch_login(&parsed.login.0)
        || !Scope::Many(parsed.scopes).valid()
    {
        return Err(invalid(op));
    }
    Ok(TwitchValidatedIdentity {
        user_id: parsed.user_id.0,
        login: parsed.login.0,
        expires_in: positive_duration(parsed.expires_in).ok_or_else(|| invalid(op))?,
        scopes: vec![TWITCH_CHAT_SCOPE.into()],
    })
}
#[derive(Deserialize)]
struct Chat {
    data: List<Object<ChatItem>, 1>,
}
#[derive(Deserialize)]
struct ChatItem {
    is_sent: bool,
    message_id: Option<Text<256>>,
    drop_reason: Option<Object<DropReason>>,
}
#[derive(Deserialize)]
struct DropReason {
    code: Text<128>,
    message: Text<1024>,
}
pub(super) fn chat(response: &HttpResponse) -> Result<TwitchDeliveryReceipt, TwitchError> {
    let op = TwitchOperation::SendChat;
    let parsed: Chat =
        json::decode(response.body.expose(), MAX_RESPONSE_BYTES).map_err(|_| invalid(op))?;
    let item = parsed
        .data
        .0
        .into_iter()
        .next()
        .ok_or_else(|| invalid(op))?
        .0;
    match (item.is_sent, item.message_id, item.drop_reason) {
        (true, Some(id), None)
            if !id.0.is_empty() && id.0.bytes().all(|b| (0x21..=0x7e).contains(&b)) =>
        {
            Ok(TwitchDeliveryReceipt {
                status: TwitchDeliveryStatus::Sent,
                message_id: Some(id.0),
            })
        }
        (false, None, Some(Object(reason)))
            if visible(&reason.code.0) && visible(&reason.message.0) =>
        {
            Ok(TwitchDeliveryReceipt {
                status: drop_status(&reason.code.0),
                message_id: None,
            })
        }
        _ => Err(invalid(op)),
    }
}
fn drop_status(code: &str) -> TwitchDeliveryStatus {
    match code {
        "automod_held" | "automod_caught" => TwitchDeliveryStatus::AutoMod,
        "blocked_term" => TwitchDeliveryStatus::BlockedTerm,
        "msg_duplicate" => TwitchDeliveryStatus::Duplicate,
        "rate_limited" => TwitchDeliveryStatus::RateLimited,
        "followers_only" => TwitchDeliveryStatus::FollowersOnly,
        "slow_mode" => TwitchDeliveryStatus::SlowMode,
        "subscribers_only" => TwitchDeliveryStatus::SubscribersOnly,
        "banned" | "channel_suspended" | "restricted" => TwitchDeliveryStatus::Restricted,
        _ => TwitchDeliveryStatus::OtherDrop,
    }
}
