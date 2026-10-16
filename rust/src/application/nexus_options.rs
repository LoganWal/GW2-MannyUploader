use super::{
    ConfigurationError, ConfigurationSnapshot, DonBotConfigurationError,
    DonBotConfigurationSnapshot, TwitchAuthenticationControllerError, TwitchConnectionSnapshot,
    TwitchConnectionState, TwitchTestMessageError, TwitchTestMessageSnapshot,
};
use crate::domain::{GeneralSettings, Settings, SettingsValidationError};

/// A draft owns only ordinary fields. Destination and verified routing state stay application-owned.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NexusOrdinaryOptions {
    pub general: GeneralSettings,
    pub twitch_client_id: String,
    pub twitch_message_template: String,
    pub twitch_post_success: bool,
    pub twitch_post_failure: bool,
}
impl From<&Settings> for NexusOrdinaryOptions {
    fn from(settings: &Settings) -> Self {
        Self {
            general: settings.general.clone(),
            twitch_client_id: settings.twitch.client_id.clone(),
            twitch_message_template: settings.twitch.message_template.clone(),
            twitch_post_success: settings.twitch.post_success,
            twitch_post_failure: settings.twitch.post_failure,
        }
    }
}
impl NexusOrdinaryOptions {
    /// Merge against the current settings at execution time, before the durable save.
    pub fn merge(
        &self,
        current: &Settings,
        connection: TwitchConnectionState,
    ) -> Result<Settings, NexusOptionsError> {
        if self.twitch_client_id != current.twitch.client_id
            && !matches!(
                connection,
                TwitchConnectionState::Disconnected | TwitchConnectionState::Error
            )
        {
            return Err(NexusOptionsError::ClientIdInUse);
        }
        let mut merged = current.clone();
        merged.general = self.general.clone();
        merged.twitch.client_id = self.twitch_client_id.clone();
        merged.twitch.message_template = self.twitch_message_template.clone();
        merged.twitch.post_success = self.twitch_post_success;
        merged.twitch.post_failure = self.twitch_post_failure;
        let errors = merged.validate();
        if errors.is_empty() {
            Ok(merged)
        } else {
            Err(NexusOptionsError::InvalidSettings(errors))
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NexusOptionsError {
    InvalidConfiguration,
    InvalidCommand,
    InvalidSettings(Vec<SettingsValidationError>),
    ClientIdInUse,
    QueueFull,
    Busy,
    Configuration(ConfigurationError),
    DonBot(DonBotConfigurationError),
    Twitch(TwitchAuthenticationControllerError),
    TestMessage(TwitchTestMessageError),
    AuthorizationUnavailable,
    BrowserLaunchFailed,
    ShuttingDown,
    RevisionExhausted,
    Internal,
}
impl NexusOptionsError {
    pub fn detail(&self) -> &'static str {
        match self {
            Self::InvalidConfiguration => "The options command queue is configured incorrectly",
            Self::InvalidCommand => "This options command is invalid",
            Self::InvalidSettings(_) => "Check the ordinary settings before saving",
            Self::ClientIdInUse => "Disconnect Twitch before changing its application Client ID",
            Self::QueueFull => "The options command queue is full",
            Self::Busy => "Options commands are already being processed",
            Self::Configuration(_) => "The configuration change could not be saved",
            Self::DonBot(error) => error.detail(),
            Self::Twitch(error) => error.detail(),
            Self::TestMessage(error) => error.detail(),
            Self::AuthorizationUnavailable => "Twitch authorization is no longer available",
            Self::BrowserLaunchFailed => {
                "The authorization page could not be opened. Check your default browser"
            }
            Self::ShuttingDown => "Options are shutting down",
            Self::RevisionExhausted => "Options cannot accept more changes",
            Self::Internal => "The options action could not be completed",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NexusOptionsSnapshot {
    pub configuration: ConfigurationSnapshot,
    pub donbot: DonBotConfigurationSnapshot,
    pub twitch: TwitchConnectionSnapshot,
    pub twitch_test_message: TwitchTestMessageSnapshot,
    pub last_error: Option<NexusOptionsError>,
    pub pending_commands: usize,
    pub revision: u64,
    pub accepting_commands: bool,
    pub shutting_down: bool,
}
