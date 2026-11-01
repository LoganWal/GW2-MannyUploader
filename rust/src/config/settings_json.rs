use std::io::Write;

use serde::{Deserialize, Serialize};

use crate::domain::{
    DonBotSettings, DpsReportSettings, GeneralSettings, Settings, SettingsValidationError,
    TwitchSettings, WingmanSettings,
};

pub const MAX_SETTINGS_BYTES: usize = 64 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SettingsDocumentError {
    TooLarge,
    InvalidJson,
    InvalidSettings(Vec<SettingsValidationError>),
    SerializationFailed,
}

pub fn decode_settings(document: &[u8]) -> Result<Settings, SettingsDocumentError> {
    if document.len() > MAX_SETTINGS_BYTES {
        return Err(SettingsDocumentError::TooLarge);
    }
    check_object_shape(document)?;
    let mut decoder = serde_json::Deserializer::from_slice(document);
    // Decode the original bytes so duplicate keys remain errors, including duplicate object keys.
    let mut settings =
        SettingsJson::deserialize(&mut decoder).map_err(|_| SettingsDocumentError::InvalidJson)?;
    decoder
        .end()
        .map_err(|_| SettingsDocumentError::InvalidJson)?;
    settings.normalize_loaded_settings();
    validate(&settings)?;
    Ok(settings)
}

pub fn encode_settings(settings: &Settings) -> Result<Vec<u8>, SettingsDocumentError> {
    validate(settings)?;
    let mut output = BoundedDocument {
        bytes: Vec::new(),
        too_large: false,
    };
    let encoded =
        SettingsJson::serialize(settings, &mut serde_json::Serializer::pretty(&mut output));
    if output.too_large {
        return Err(SettingsDocumentError::TooLarge);
    }
    encoded.map_err(|_| SettingsDocumentError::SerializationFailed)?;
    output
        .write_all(b"\n")
        .map_err(|_| SettingsDocumentError::TooLarge)?;
    Ok(output.bytes)
}

fn validate(settings: &Settings) -> Result<(), SettingsDocumentError> {
    let errors = settings.validate();
    if errors.is_empty() {
        Ok(())
    } else {
        Err(SettingsDocumentError::InvalidSettings(errors))
    }
}

fn check_object_shape(document: &[u8]) -> Result<(), SettingsDocumentError> {
    // Serde structs also accept positional arrays. Version 1 requires named JSON objects. This
    // bounded shape check is discarded before typed decoding, which enforces duplicates and keys.
    let value: serde_json::Value =
        serde_json::from_slice(document).map_err(|_| SettingsDocumentError::InvalidJson)?;
    let object = value
        .as_object()
        .ok_or(SettingsDocumentError::InvalidJson)?;
    for key in ["general", "dps_report", "wingman", "donbot", "twitch"] {
        if object.get(key).is_some_and(|value| !value.is_object()) {
            return Err(SettingsDocumentError::InvalidJson);
        }
    }
    Ok(())
}

struct BoundedDocument {
    bytes: Vec<u8>,
    too_large: bool,
}
impl Write for BoundedDocument {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        if bytes.len() > MAX_SETTINGS_BYTES - self.bytes.len() {
            self.too_large = true;
            return Err(std::io::Error::other("settings size limit"));
        }
        self.bytes
            .try_reserve(bytes.len())
            .map_err(|_| std::io::Error::other("settings allocation failed"))?;
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

#[derive(Serialize, Deserialize)]
#[serde(remote = "Settings", deny_unknown_fields)]
struct SettingsJson {
    schema_version: u32,
    #[serde(default, with = "GeneralJson")]
    general: GeneralSettings,
    #[serde(default, with = "DpsReportJson")]
    dps_report: DpsReportSettings,
    #[serde(default, with = "WingmanJson")]
    wingman: WingmanSettings,
    #[serde(default, with = "DonBotJson")]
    donbot: DonBotSettings,
    #[serde(default, with = "TwitchJson")]
    twitch: TwitchSettings,
}

#[derive(Serialize, Deserialize)]
#[serde(
    remote = "GeneralSettings",
    default = "GeneralSettings::default",
    deny_unknown_fields
)]
struct GeneralJson {
    log_directory: String,
    watch_subdirectories: bool,
    window_visible: bool,
    poll_interval_ms: u32,
    stability_observations: u32,
    recent_log_limit: u32,
    parser_queue_capacity: u32,
    parallel_uploads_per_provider: u32,
    max_candidates: u32,
}

#[derive(Serialize, Deserialize)]
#[serde(
    remote = "DpsReportSettings",
    default = "DpsReportSettings::default",
    deny_unknown_fields
)]
struct DpsReportJson {
    enabled: bool,
    detailed_wvw: bool,
}

#[derive(Serialize, Deserialize)]
#[serde(
    remote = "WingmanSettings",
    default = "WingmanSettings::default",
    deny_unknown_fields
)]
struct WingmanJson {
    enabled: bool,
}

#[derive(Serialize, Deserialize)]
#[serde(
    remote = "DonBotSettings",
    default = "DonBotSettings::default",
    deny_unknown_fields
)]
struct DonBotJson {
    enabled: bool,
    api_base_url: String,
    selected_guild_id: String,
    discord_delivery_enabled: bool,
    discord_channel_override_explicit: bool,
    selected_discord_channel_id: String,
}

#[derive(Serialize, Deserialize)]
#[serde(
    remote = "TwitchSettings",
    default = "TwitchSettings::default",
    deny_unknown_fields
)]
struct TwitchJson {
    enabled: bool,
    client_id: String,
    message_template: String,
    post_success: bool,
    post_failure: bool,
}
