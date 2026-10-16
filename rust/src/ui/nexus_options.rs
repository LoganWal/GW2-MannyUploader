use crate::application::*;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotOptionsModel {
    pub status_text: String,
    pub diagnostic: Option<&'static str>,
    pub discord_delivery_status_text: &'static str,
    pub verify_available: bool,
    pub verification_in_progress: bool,
    pub configuration_visible: bool,
    pub guild_selection_available: bool,
    pub enable_toggle_available: bool,
    pub refresh_available: bool,
    pub discord_delivery_visible: bool,
    pub discord_delivery_toggle_available: bool,
    pub discord_channel_selection_visible: bool,
    pub discord_channel_selection_available: bool,
    pub disconnect_available: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchOptionsModel {
    pub status_text: String,
    pub diagnostic: Option<&'static str>,
    pub test_message_status_text: &'static str,
    pub test_message_diagnostic: Option<&'static str>,
    pub masked_user_code: Option<&'static str>,
    pub connect_available: bool,
    pub authorization_available: bool,
    pub enable_toggle_available: bool,
    pub disconnect_available: bool,
    pub test_message_available: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NexusOptionsModel {
    pub ordinary: NexusOrdinaryOptions,
    pub donbot: DonBotOptionsModel,
    pub twitch: TwitchOptionsModel,
    pub protected_storage_text: &'static str,
    pub last_error: Option<&'static str>,
    pub settings_editable: bool,
    pub save_available: bool,
    pub command_pending: bool,
}

/// Prevent provider labels from introducing ImGui's hidden-label or stable-ID syntax.
pub fn escape_imgui_label_text(text: &str) -> String {
    let mut result = String::with_capacity(text.len());
    let mut previous_hash = false;
    for character in text.chars() {
        if character == '#' && previous_hash {
            result.push(' ');
        }
        result.push(character);
        previous_hash = character == '#';
    }
    result
}

pub fn build_nexus_options_model(snapshot: &NexusOptionsSnapshot) -> NexusOptionsModel {
    let active = snapshot.accepting_commands
        && !snapshot.shutting_down
        && !snapshot.configuration.shutting_down;
    let storage = snapshot.configuration.persistent_secret_storage;
    let protected = storage.state == PersistentSecretStorageState::Available;
    NexusOptionsModel {
        ordinary: NexusOrdinaryOptions::from(&snapshot.configuration.settings),
        donbot: donbot_model(snapshot, active, protected),
        twitch: twitch_model(snapshot, active, protected),
        protected_storage_text: match (storage.state, storage.mode) {
            (
                PersistentSecretStorageState::Available,
                Some(SecretProtectionMode::WineCompatibility),
            ) => "Wine compatibility credential storage provides reduced protection",
            (PersistentSecretStorageState::Available, _) => {
                "Protected credential storage is available"
            }
            (PersistentSecretStorageState::UnsupportedEnvironment, _) => {
                "Protected credential storage is unavailable in this environment"
            }
            (PersistentSecretStorageState::InitializationFailed, _) => {
                "Protected credential storage could not be initialized"
            }
        },
        last_error: snapshot.last_error.as_ref().map(NexusOptionsError::detail),
        settings_editable: active,
        save_available: active,
        command_pending: snapshot.pending_commands != 0,
    }
}

fn donbot_model(
    snapshot: &NexusOptionsSnapshot,
    active: bool,
    protected: bool,
) -> DonBotOptionsModel {
    use DonBotConfigurationState as State;
    let bot = &snapshot.donbot;
    let settings = &snapshot.configuration.settings.donbot;
    let verified = bot.state == State::Verified && bot.account_name.is_some();
    let idle = !bot.shutting_down
        && !bot.refreshing
        && !matches!(bot.state, State::Verifying | State::ShuttingDown);
    let mutable = active && idle;
    let policy = bot
        .guilds
        .iter()
        .find(|guild| guild.guild_id == bot.selected_guild_id)
        .map(|guild| &guild.discord_delivery);
    let summary = policy.is_some_and(|policy| {
        policy.pve_summary || policy.wvw_summary || policy.wvw_advanced || policy.wvw_stream
    });
    let route = policy.is_some_and(|policy| {
        policy.defaults_available
            || (policy.channel_override_allowed && !policy.channels.is_empty())
    });
    let summary_visible = settings.enabled
        && verified
        && bot.discord_summary_delivery_v1
        && policy.is_some_and(|policy| policy.enabled)
        && summary
        && route;
    let aggregate_route = verified
        && bot.discord_aggregate_delivery_v1
        && policy.is_some_and(|policy| {
            policy.enabled
                && policy.aggregate_enabled
                && (policy.aggregate_defaults_available
                    || (policy.channel_override_allowed && !policy.channels.is_empty()))
        });
    let channel_visible = summary_visible || (settings.enabled && aggregate_route);
    let reason = if !settings.enabled || summary_visible {
        ""
    } else if !verified {
        "Verify DonBot to configure Discord summaries"
    } else if let Some(policy) = policy {
        if !bot.discord_summary_delivery_v1 {
            "This DonBot deployment does not support Discord summaries"
        } else if !policy.enabled {
            "Discord summaries are disabled in this server's DonBot settings"
        } else if !summary {
            "No Discord summary outputs are enabled in this server's DonBot settings"
        } else {
            "No authorized Discord log destination is available"
        }
    } else {
        "Select a DonBot server to configure Discord summaries"
    };
    let mut status = match bot.state {
        State::Unverified => "Not verified".into(),
        State::Verifying => "Verifying DonBot account".into(),
        State::Verified => bot
            .account_name
            .as_ref()
            .map_or_else(|| "Verified".into(), |name| format!("Verified as {name}")),
        State::Error => "DonBot verification error".into(),
        State::ShuttingDown => "Shutting down".into(),
    };
    if bot.refreshing && verified {
        status.push_str(" (refreshing server settings)");
    }
    DonBotOptionsModel {
        status_text: status,
        diagnostic: bot.diagnostic.map(DonBotConfigurationError::detail),
        discord_delivery_status_text: reason,
        verify_available: mutable && protected && !verified,
        verification_in_progress: bot.state == State::Verifying,
        configuration_visible: settings.enabled,
        guild_selection_available: mutable && verified && !bot.guilds.is_empty(),
        enable_toggle_available: mutable && (settings.enabled || (verified && policy.is_some())),
        refresh_available: mutable && protected && verified,
        discord_delivery_visible: summary_visible,
        discord_delivery_toggle_available: mutable && summary_visible,
        discord_channel_selection_visible: channel_visible,
        discord_channel_selection_available: mutable && channel_visible,
        disconnect_available: mutable
            && (bot.state != State::Unverified
                || settings.enabled
                || !settings.selected_guild_id.is_empty()),
    }
}

fn twitch_model(
    snapshot: &NexusOptionsSnapshot,
    active: bool,
    protected: bool,
) -> TwitchOptionsModel {
    use TwitchConnectionState as State;
    use TwitchTestMessageState as Test;
    let twitch = &snapshot.twitch;
    let test = &snapshot.twitch_test_message;
    let active = active && !twitch.shutting_down && twitch.state != State::ShuttingDown;
    let connected = twitch.state == State::Connected;
    TwitchOptionsModel {
        status_text: match twitch.state {
            State::Disconnected => "Not connected".into(),
            State::Starting => "Starting Twitch connection".into(),
            State::AwaitingUser => "Waiting for Twitch authorization".into(),
            State::Validating => "Validating broadcaster account".into(),
            State::Refreshing => "Refreshing Twitch connection".into(),
            State::Connected => twitch.login.as_ref().map_or_else(
                || "Connected".into(),
                |login| format!("Connected as {login}"),
            ),
            State::Disconnecting => "Disconnecting Twitch".into(),
            State::Error => "Twitch connection error".into(),
            State::ShuttingDown => "Shutting down".into(),
        },
        diagnostic: twitch
            .diagnostic
            .map(TwitchAuthenticationControllerError::detail),
        test_message_status_text: match test.state {
            Test::Idle => "No test message sent",
            Test::Sending => "Sending test message",
            Test::Sent => "Test message sent",
            Test::Error => "Test message error",
            Test::ShuttingDown => "Shutting down",
        },
        test_message_diagnostic: test.diagnostic.map(TwitchTestMessageError::detail),
        masked_user_code: twitch.user_code.as_ref().map(|_| "********"),
        connect_available: active
            && protected
            && matches!(twitch.state, State::Disconnected | State::Error),
        authorization_available: active
            && twitch.state == State::AwaitingUser
            && twitch.verification_uri.is_some(),
        enable_toggle_available: active
            && (snapshot.configuration.settings.twitch.enabled || connected),
        disconnect_available: active
            && !matches!(twitch.state, State::Disconnected | State::Disconnecting),
        test_message_available: active
            && connected
            && !matches!(test.state, Test::Sending | Test::ShuttingDown),
    }
}
