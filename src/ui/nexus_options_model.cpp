#include "manny_uploader/ui/nexus_options_model.hpp"

namespace manny_uploader::ui {
namespace {

[[nodiscard]] std::string protected_storage_text(application::PersistentSecretStorageState state) {
    switch (state) {
    case application::PersistentSecretStorageState::Available:
        return "Protected credential storage is available";
    case application::PersistentSecretStorageState::UnsupportedEnvironment:
        return "Protected credential storage is unavailable in this environment";
    case application::PersistentSecretStorageState::InitializationFailed:
        return "Protected credential storage could not be initialized";
    }
    return "Protected credential storage state is unknown";
}

[[nodiscard]] std::string donbot_status(const application::DonBotConfigurationSnapshot& snapshot) {
    switch (snapshot.state) {
    case application::DonBotConfigurationState::Unverified:
        return "Not verified";
    case application::DonBotConfigurationState::Verifying:
        return "Verifying DonBot account";
    case application::DonBotConfigurationState::Verified:
        return snapshot.account_name ? "Verified as " + *snapshot.account_name : "Verified";
    case application::DonBotConfigurationState::Error:
        return "DonBot verification error";
    case application::DonBotConfigurationState::ShuttingDown:
        return "Shutting down";
    }
    return "Unknown DonBot state";
}

[[nodiscard]] std::string twitch_status(const application::TwitchConnectionSnapshot& snapshot) {
    switch (snapshot.state) {
    case application::TwitchConnectionState::Disconnected:
        return "Not connected";
    case application::TwitchConnectionState::Starting:
        return "Starting Twitch connection";
    case application::TwitchConnectionState::AwaitingUser:
        return "Waiting for Twitch authorization";
    case application::TwitchConnectionState::Validating:
        return "Validating broadcaster account";
    case application::TwitchConnectionState::Refreshing:
        return "Refreshing Twitch connection";
    case application::TwitchConnectionState::Connected:
        return snapshot.login ? "Connected as " + *snapshot.login : "Connected";
    case application::TwitchConnectionState::Disconnecting:
        return "Disconnecting Twitch";
    case application::TwitchConnectionState::Error:
        return "Twitch connection error";
    case application::TwitchConnectionState::ShuttingDown:
        return "Shutting down";
    }
    return "Unknown Twitch state";
}

[[nodiscard]] std::string
twitch_test_message_status(const application::TwitchTestMessageSnapshot& snapshot) {
    switch (snapshot.state) {
    case application::TwitchTestMessageState::Idle:
        return "No test message sent";
    case application::TwitchTestMessageState::Sending:
        return "Sending test message";
    case application::TwitchTestMessageState::Sent:
        return "Test message sent";
    case application::TwitchTestMessageState::Error:
        return "Test message error";
    case application::TwitchTestMessageState::ShuttingDown:
        return "Shutting down";
    }
    return "Unknown test-message state";
}

} // namespace

NexusOptionsModel build_nexus_options_model(const application::NexusOptionsSnapshot& snapshot) {
    const auto storage_available = snapshot.configuration.persistent_secret_storage.state ==
                                   application::PersistentSecretStorageState::Available;
    const auto active = snapshot.accepting_commands && !snapshot.shutting_down;
    const auto donbot_idle =
        snapshot.donbot.state != application::DonBotConfigurationState::Verifying &&
        snapshot.donbot.state != application::DonBotConfigurationState::ShuttingDown;
    const auto donbot_verified =
        snapshot.donbot.state == application::DonBotConfigurationState::Verified &&
        snapshot.donbot.account_name.has_value();
    const auto twitch_idle =
        snapshot.twitch.state == application::TwitchConnectionState::Disconnected ||
        snapshot.twitch.state == application::TwitchConnectionState::Error;
    const auto twitch_connected =
        snapshot.twitch.state == application::TwitchConnectionState::Connected;

    return NexusOptionsModel{
        .ordinary = application::ordinary_options_from(snapshot.configuration.settings),
        .donbot =
            DonBotOptionsModel{
                .status_text = donbot_status(snapshot.donbot),
                .diagnostic = snapshot.donbot.diagnostic,
                .verify_available = active && storage_available && donbot_idle,
                .guild_selection_available =
                    active && donbot_verified && !snapshot.donbot.guilds.empty(),
                .enable_toggle_available =
                    active && (snapshot.configuration.settings.donbot.enabled ||
                               (donbot_verified && !snapshot.donbot.selected_guild_id.empty())),
                .disconnect_available =
                    active && donbot_idle &&
                    (snapshot.donbot.state != application::DonBotConfigurationState::Unverified ||
                     snapshot.configuration.settings.donbot.enabled ||
                     !snapshot.configuration.settings.donbot.selected_guild_id.empty()),
            },
        .twitch =
            TwitchOptionsModel{
                .status_text = twitch_status(snapshot.twitch),
                .diagnostic = snapshot.twitch.diagnostic,
                .test_message_status_text =
                    twitch_test_message_status(snapshot.twitch_test_message),
                .test_message_diagnostic = snapshot.twitch_test_message.diagnostic,
                .user_code = snapshot.twitch.user_code,
                .verification_uri = snapshot.twitch.verification_uri,
                .connect_available = active && storage_available && twitch_idle,
                .enable_toggle_available =
                    active && (snapshot.configuration.settings.twitch.enabled || twitch_connected),
                .disconnect_available =
                    active &&
                    snapshot.twitch.state != application::TwitchConnectionState::Disconnected &&
                    snapshot.twitch.state != application::TwitchConnectionState::Disconnecting &&
                    snapshot.twitch.state != application::TwitchConnectionState::ShuttingDown,
                .test_message_available = active && twitch_connected &&
                                          snapshot.twitch_test_message.state !=
                                              application::TwitchTestMessageState::Sending,
            },
        .protected_storage_text =
            protected_storage_text(snapshot.configuration.persistent_secret_storage.state),
        .last_error = snapshot.last_error ? std::optional<std::string>{snapshot.last_error->message}
                                          : std::nullopt,
        .settings_editable = active,
        .save_available = active,
        .command_pending = snapshot.pending_commands != 0,
    };
}

} // namespace manny_uploader::ui
