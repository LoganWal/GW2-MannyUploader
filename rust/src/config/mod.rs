mod settings_json;

pub use settings_json::{
    MAX_SETTINGS_BYTES, SettingsDocumentError, decode_settings, encode_settings,
};
