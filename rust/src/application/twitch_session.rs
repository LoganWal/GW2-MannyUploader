use std::time::{Duration, SystemTime};

use super::{MAX_SECRET_BYTES, SecretValue};

pub const TWITCH_CHAT_SCOPE: &str = "user:write:chat";
pub const MAX_TWITCH_TOKEN_BYTES: usize = 8 * 1024;
pub const MAX_TWITCH_SESSION_BYTES: usize = MAX_SECRET_BYTES;
const MAGIC: &[u8; 8] = b"MNYTWSN1";
const HEADER_BYTES: usize = 40;
const MAX_UNIX_SECONDS: u64 = 253_402_300_799;

#[derive(Debug)]
pub struct TwitchCredentialSet {
    pub access_token: SecretValue,
    pub refresh_token: SecretValue,
    pub access_expires_at: SystemTime,
}

/// One atomic protected record. Tokens stay move-only and never enter public snapshots.
pub struct TwitchSession {
    pub credentials: TwitchCredentialSet,
    pub user_id: String,
    pub login: String,
    pub scopes: Vec<String>,
}
impl std::fmt::Debug for TwitchSession {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("TwitchSession([REDACTED])")
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchSessionError {
    InvalidSession,
    ValueTooLarge,
    UnsupportedVersion,
    CorruptRecord,
}
impl TwitchSessionError {
    pub fn detail(self) -> &'static str {
        match self {
            Self::InvalidSession => "The Twitch session cannot be persisted",
            Self::ValueTooLarge => "The Twitch session has an invalid size",
            Self::UnsupportedVersion => "The saved Twitch session version is unsupported",
            Self::CorruptRecord => "The saved Twitch session is invalid",
        }
    }
}

pub fn valid_twitch_token(token: &SecretValue) -> bool {
    valid_token_bytes(token.expose())
}
fn valid_token_bytes(bytes: &[u8]) -> bool {
    !bytes.is_empty()
        && bytes.len() <= MAX_TWITCH_TOKEN_BYTES
        && bytes.iter().all(|b| (0x21..=0x7e).contains(b))
}
pub fn valid_twitch_user_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 20
        && !value.starts_with('0')
        && value.bytes().all(|b| b.is_ascii_digit())
        && value.parse::<u64>().is_ok_and(|id| id > 0)
}
pub fn valid_twitch_login(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

pub fn encode_twitch_session(session: &TwitchSession) -> Result<SecretValue, TwitchSessionError> {
    let seconds = session
        .credentials
        .access_expires_at
        .duration_since(SystemTime::UNIX_EPOCH)
        .map_err(|_| TwitchSessionError::InvalidSession)?
        .as_secs();
    if !(1..=MAX_UNIX_SECONDS).contains(&seconds)
        || !valid_twitch_token(&session.credentials.access_token)
        || !valid_twitch_token(&session.credentials.refresh_token)
        || !valid_twitch_user_id(&session.user_id)
        || !valid_twitch_login(&session.login)
        || session.scopes.len() != 1
        || session.scopes[0] != TWITCH_CHAT_SCOPE
    {
        return Err(TwitchSessionError::InvalidSession);
    }
    let fields = [
        session.credentials.access_token.expose(),
        session.credentials.refresh_token.expose(),
        session.user_id.as_bytes(),
        session.login.as_bytes(),
    ];
    // Every field is bounded above before summing or allocating. Fill one wiping allocation.
    let total =
        HEADER_BYTES + fields.iter().map(|f| f.len()).sum::<usize>() + 4 + TWITCH_CHAT_SCOPE.len();
    if total > MAX_TWITCH_SESSION_BYTES {
        return Err(TwitchSessionError::ValueTooLarge);
    }
    let mut record = SecretValue::from_bytes(vec![0; total]);
    let bytes = record.expose_mut();
    bytes[..8].copy_from_slice(MAGIC);
    bytes[8..12].copy_from_slice(&1u32.to_le_bytes());
    bytes[12..20].copy_from_slice(&(seconds as i64).to_le_bytes());
    for (index, field) in fields.iter().enumerate() {
        bytes[20 + index * 4..24 + index * 4].copy_from_slice(&(field.len() as u32).to_le_bytes());
    }
    bytes[36..40].copy_from_slice(&1u32.to_le_bytes());
    let mut cursor = HEADER_BYTES;
    for field in fields {
        bytes[cursor..cursor + field.len()].copy_from_slice(field);
        cursor += field.len();
    }
    bytes[cursor..cursor + 4].copy_from_slice(&(TWITCH_CHAT_SCOPE.len() as u32).to_le_bytes());
    bytes[cursor + 4..].copy_from_slice(TWITCH_CHAT_SCOPE.as_bytes());
    Ok(record)
}

pub fn decode_twitch_session(record: &SecretValue) -> Result<TwitchSession, TwitchSessionError> {
    use TwitchSessionError::{CorruptRecord, UnsupportedVersion, ValueTooLarge};
    let bytes = record.expose();
    if bytes.is_empty() || bytes.len() > MAX_TWITCH_SESSION_BYTES {
        return Err(ValueTooLarge);
    }
    if bytes.len() < 12 || &bytes[..8] != MAGIC {
        return Err(CorruptRecord);
    }
    if bytes[8..12] != 1u32.to_le_bytes() {
        return Err(UnsupportedVersion);
    }
    if bytes.len() < HEADER_BYTES {
        return Err(CorruptRecord);
    }
    let seconds = i64::from_le_bytes(bytes[12..20].try_into().map_err(|_| CorruptRecord)?);
    if seconds <= 0 || seconds as u64 > MAX_UNIX_SECONDS || bytes[36..40] != 1u32.to_le_bytes() {
        return Err(CorruptRecord);
    }
    let mut fields: [&[u8]; 4] = [&[]; 4];
    let mut remaining = &bytes[HEADER_BYTES..];
    for (index, field) in fields.iter_mut().enumerate() {
        let length = u32::from_le_bytes(
            bytes[20 + index * 4..24 + index * 4]
                .try_into()
                .map_err(|_| CorruptRecord)?,
        ) as usize;
        if length > remaining.len() {
            return Err(CorruptRecord);
        }
        (*field, remaining) = remaining.split_at(length);
    }
    if remaining.len() != 4 + TWITCH_CHAT_SCOPE.len()
        || remaining[..4] != (TWITCH_CHAT_SCOPE.len() as u32).to_le_bytes()
        || &remaining[4..] != TWITCH_CHAT_SCOPE.as_bytes()
        || !valid_token_bytes(fields[0])
        || !valid_token_bytes(fields[1])
    {
        return Err(CorruptRecord);
    }
    let user_id = std::str::from_utf8(fields[2]).map_err(|_| CorruptRecord)?;
    let login = std::str::from_utf8(fields[3]).map_err(|_| CorruptRecord)?;
    if !valid_twitch_user_id(user_id) || !valid_twitch_login(login) {
        return Err(CorruptRecord);
    }
    let access_expires_at = SystemTime::UNIX_EPOCH
        .checked_add(Duration::from_secs(seconds as u64))
        .ok_or(CorruptRecord)?;
    // Validate the entire borrowed payload before allocating any decoded field.
    Ok(TwitchSession {
        credentials: TwitchCredentialSet {
            access_token: SecretValue::from_bytes(fields[0].to_vec()),
            refresh_token: SecretValue::from_bytes(fields[1].to_vec()),
            access_expires_at,
        },
        user_id: user_id.to_owned(),
        login: login.to_owned(),
        scopes: vec![TWITCH_CHAT_SCOPE.into()],
    })
}
