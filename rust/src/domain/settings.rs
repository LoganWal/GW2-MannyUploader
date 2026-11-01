use super::receipts::canonical_discord_id;
use super::{ProviderSelection, TemplateError, TwitchMessageTemplate};

pub const SETTINGS_SCHEMA_VERSION: u32 = 1;
pub const DEFAULT_DONBOT_API: &str = "https://donbot-api.walmslo.com";
pub const DEFAULT_TWITCH_TEMPLATE: &str = "{encounter}{mode_suffix} - {result}: {url}";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GeneralSettings {
    pub log_directory: String,
    pub watch_subdirectories: bool,
    pub window_visible: bool,
    pub poll_interval_ms: u32,
    pub stability_observations: u32,
    pub recent_log_limit: u32,
    pub parser_queue_capacity: u32,
    pub parallel_uploads_per_provider: u32,
    pub max_candidates: u32,
}
impl Default for GeneralSettings {
    fn default() -> Self {
        Self {
            log_directory: String::new(),
            watch_subdirectories: true,
            window_visible: true,
            poll_interval_ms: 1000,
            stability_observations: 2,
            recent_log_limit: 100,
            parser_queue_capacity: 8,
            parallel_uploads_per_provider: 5,
            max_candidates: 4096,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DpsReportSettings {
    pub enabled: bool,
    pub detailed_wvw: bool,
}
impl Default for DpsReportSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            detailed_wvw: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WingmanSettings {
    pub enabled: bool,
}
impl Default for WingmanSettings {
    fn default() -> Self {
        Self { enabled: true }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotSettings {
    pub enabled: bool,
    pub api_base_url: String,
    pub selected_guild_id: String,
    pub discord_delivery_enabled: bool,
    pub discord_channel_override_explicit: bool,
    pub selected_discord_channel_id: String,
}
impl Default for DonBotSettings {
    fn default() -> Self {
        Self {
            enabled: false,
            api_base_url: DEFAULT_DONBOT_API.into(),
            selected_guild_id: String::new(),
            discord_delivery_enabled: false,
            discord_channel_override_explicit: false,
            selected_discord_channel_id: String::new(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchSettings {
    pub enabled: bool,
    pub client_id: String,
    pub message_template: String,
    pub post_success: bool,
    pub post_failure: bool,
}
impl Default for TwitchSettings {
    fn default() -> Self {
        Self {
            enabled: false,
            client_id: String::new(),
            message_template: DEFAULT_TWITCH_TEMPLATE.into(),
            post_success: true,
            post_failure: true,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Settings {
    pub schema_version: u32,
    pub general: GeneralSettings,
    pub dps_report: DpsReportSettings,
    pub wingman: WingmanSettings,
    pub donbot: DonBotSettings,
    pub twitch: TwitchSettings,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SettingsErrorCode {
    UnsupportedSchema,
    EmptyValue,
    InvalidPath,
    ValueTooLong,
    OutOfRange,
    InvalidUrl,
    InvalidGuildId,
    InvalidChannelId,
    DiscordRequiresDonBot,
    InvalidTwitchClientId,
    TwitchRequiresDpsReport,
    TwitchPostingDisabled,
    Template(TemplateError),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SettingsValidationError {
    pub field: &'static str,
    pub code: SettingsErrorCode,
}

impl Settings {
    pub fn new(log_directory: String) -> Self {
        Self {
            schema_version: SETTINGS_SCHEMA_VERSION,
            general: GeneralSettings {
                log_directory,
                ..Default::default()
            },
            dps_report: DpsReportSettings::default(),
            wingman: WingmanSettings::default(),
            donbot: DonBotSettings::default(),
            twitch: TwitchSettings::default(),
        }
    }

    pub fn enabled_providers(&self) -> ProviderSelection {
        ProviderSelection {
            dps_report: self.dps_report.enabled,
            wingman: self.wingman.enabled,
            donbot: self.donbot.enabled,
            twitch: self.twitch.enabled,
        }
    }

    /// Loading clears stale legacy channel choices before validation. Save validation never mutates.
    pub fn normalize_loaded_settings(&mut self) {
        if !self.donbot.enabled || !self.donbot.discord_channel_override_explicit {
            self.donbot.discord_channel_override_explicit = false;
            self.donbot.selected_discord_channel_id.clear();
        }
    }

    pub fn validate(&self) -> Vec<SettingsValidationError> {
        use SettingsErrorCode::*;
        let mut errors = Vec::new();
        let mut reject = |field, code| errors.push(SettingsValidationError { field, code });
        if self.schema_version != SETTINGS_SCHEMA_VERSION {
            reject("schema_version", UnsupportedSchema);
        }
        let path = &self.general.log_directory;
        if path.is_empty() {
            reject("general.log_directory", EmptyValue);
        }
        if path.len() > 4096 {
            reject("general.log_directory", ValueTooLong);
        }
        if path.bytes().any(|byte| byte < 0x20) {
            reject("general.log_directory", InvalidPath);
        }
        for (field, value, min, max) in [
            (
                "general.poll_interval_ms",
                self.general.poll_interval_ms,
                250,
                60_000,
            ),
            (
                "general.stability_observations",
                self.general.stability_observations,
                2,
                10,
            ),
            (
                "general.recent_log_limit",
                self.general.recent_log_limit,
                1,
                500,
            ),
            (
                "general.parser_queue_capacity",
                self.general.parser_queue_capacity,
                1,
                64,
            ),
            (
                "general.parallel_uploads_per_provider",
                self.general.parallel_uploads_per_provider,
                1,
                32,
            ),
            (
                "general.max_candidates",
                self.general.max_candidates,
                1,
                10_000,
            ),
        ] {
            if !(min..=max).contains(&value) {
                reject(field, OutOfRange);
            }
        }
        if !valid_api_base(&self.donbot.api_base_url) {
            reject("donbot.api_base_url", InvalidUrl);
        }
        if !self.donbot.selected_guild_id.is_empty()
            && !canonical_discord_id(&self.donbot.selected_guild_id)
        {
            reject("donbot.selected_guild_id", InvalidGuildId);
        }
        if self.donbot.enabled && self.donbot.selected_guild_id.is_empty() {
            reject("donbot.selected_guild_id", EmptyValue);
        }
        if !self.donbot.selected_discord_channel_id.is_empty()
            && !canonical_discord_id(&self.donbot.selected_discord_channel_id)
        {
            reject("donbot.selected_discord_channel_id", InvalidChannelId);
        }
        if self.donbot.discord_delivery_enabled && !self.donbot.enabled {
            reject("donbot.discord_delivery_enabled", DiscordRequiresDonBot);
        }
        if self.twitch.client_id.len() > 128
            || !self
                .twitch
                .client_id
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        {
            reject("twitch.client_id", InvalidTwitchClientId);
        }
        if let Err(error) = TwitchMessageTemplate::parse(&self.twitch.message_template) {
            reject("twitch.message_template", Template(error));
        }
        if self.twitch.enabled && !self.dps_report.enabled {
            reject("twitch.enabled", TwitchRequiresDpsReport);
        }
        if self.twitch.enabled && !self.twitch.post_success && !self.twitch.post_failure {
            reject("twitch.enabled", TwitchPostingDisabled);
        }
        errors
    }
}

fn valid_api_base(value: &str) -> bool {
    let Some(remainder) = value.strip_prefix("https://") else {
        return false;
    };
    if value.len() > 2048
        || value
            .chars()
            .any(|ch| ch.is_whitespace() || ch.is_ascii_control())
        || value.contains(['@', '?', '#', '\\'])
    {
        return false;
    }
    let (authority, path) = remainder.split_once('/').unwrap_or((remainder, ""));
    if authority.is_empty() || path.split('/').any(|segment| matches!(segment, "." | "..")) {
        return false;
    }
    if let Some(ipv6) = authority.strip_prefix('[') {
        let Some((host, suffix)) = ipv6.split_once(']') else {
            return false;
        };
        host.parse::<std::net::Ipv6Addr>().is_ok()
            && (suffix.is_empty() || suffix.strip_prefix(':').is_some_and(valid_port))
    } else {
        let (host, port) = authority
            .split_once(':')
            .map_or((authority, None), |(host, port)| (host, Some(port)));
        !host.is_empty() && !host.contains(['[', ']', '%']) && port.is_none_or(valid_port)
    }
}

fn valid_port(port: &str) -> bool {
    !port.is_empty()
        && port.bytes().all(|b| b.is_ascii_digit())
        && port.parse::<u16>().is_ok_and(|p| p != 0)
}
