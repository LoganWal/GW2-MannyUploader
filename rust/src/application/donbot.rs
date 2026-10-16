use super::{HttpTransportPolicy, SecretValue, validate_http_url};

pub const MAX_DONBOT_API_KEY_BYTES: usize = 512;
pub const MAX_DONBOT_GUILDS: usize = 256;
pub const MAX_DONBOT_CHANNELS_PER_GUILD: usize = 256;
pub const MAX_DONBOT_CHANNELS: usize = 2048;
pub const MAX_DONBOT_AGGREGATE_FIGHTS: u16 = 100;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotChannel {
    pub channel_id: String,
    pub channel_name: String,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DonBotDiscordDeliveryPolicy {
    pub enabled: bool,
    pub defaults_available: bool,
    pub channel_override_allowed: bool,
    pub pve_summary: bool,
    pub wvw_summary: bool,
    pub wvw_advanced: bool,
    pub wvw_stream: bool,
    pub aggregate_enabled: bool,
    pub aggregate_defaults_available: bool,
    pub max_aggregate_fight_logs: u16,
    pub channels: Vec<DonBotChannel>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotGuild {
    pub guild_id: String,
    pub guild_name: String,
    pub discord_delivery: DonBotDiscordDeliveryPolicy,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotVerification {
    pub account_name: String,
    pub discord_summary_delivery_v1: bool,
    pub discord_aggregate_delivery_v1: bool,
    pub guilds: Vec<DonBotGuild>,
}

pub struct DonBotVerificationRequest {
    pub request_id: u64,
    pub api_base_url: String,
    pub api_key: SecretValue,
}
impl std::fmt::Debug for DonBotVerificationRequest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DonBotVerificationRequest")
            .field("request_id", &self.request_id)
            .finish_non_exhaustive()
    }
}
#[derive(Debug)]
pub struct DonBotVerificationSuccess {
    pub identity: DonBotVerification,
    pub api_key: SecretValue,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotVerificationFailure {
    Failed,
    Cancelled,
}
#[derive(Debug)]
pub struct DonBotVerificationResult {
    pub request_id: u64,
    pub verification: Result<DonBotVerificationSuccess, DonBotVerificationFailure>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotVerificationDispatchError {
    InvalidRequest,
    QueueFull,
    Stopping,
}
pub trait DonBotVerifier: Send {
    fn enqueue(
        &mut self,
        request: DonBotVerificationRequest,
    ) -> Result<(), DonBotVerificationDispatchError>;
    fn try_take_result(&mut self) -> Option<DonBotVerificationResult>;
    fn cancel_pending(&mut self);
}

pub fn valid_donbot_api_key(key: &SecretValue) -> bool {
    !key.is_empty()
        && key.len() <= MAX_DONBOT_API_KEY_BYTES
        && key
            .expose()
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || *byte == b'-')
}

/// Shared by verification admission and the HTTP client. No network or credential I/O.
pub fn normalize_donbot_api_base(value: &str) -> Option<String> {
    if value.len() > 2048
        || !value.bytes().all(|b| (0x21..=0x7e).contains(&b))
        || value.contains(['@', '?', '#', '\\', '"'])
        || validate_http_url(value, HttpTransportPolicy::HttpsOnly).is_err()
    {
        return None;
    }
    let rest = value.strip_prefix("https://")?;
    let (authority, path) = rest.split_once('/').unwrap_or((rest, ""));
    if authority.ends_with('.') || path.split('/').any(|part| matches!(part, "." | "..")) {
        return None;
    }
    Some(value.trim_end_matches('/').to_owned())
}
