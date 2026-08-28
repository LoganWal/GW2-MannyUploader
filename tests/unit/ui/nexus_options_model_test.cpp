#include "manny_uploader/ui/nexus_options_model.hpp"

#include "support/test_suite.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace manny_uploader::test {
namespace {

[[nodiscard]] application::NexusOptionsSnapshot snapshot() {
    return application::NexusOptionsSnapshot{
        .configuration =
            application::ConfigurationSnapshot{
                .settings = config::make_default_settings("C:/logs"),
                .settings_load_source = ports::SettingsLoadSource::Primary,
                .settings_recovery_diagnostic = std::nullopt,
                .persistent_secret_storage =
                    application::PersistentSecretStorageSnapshot{
                        .state = application::PersistentSecretStorageState::Available,
                        .error_code = std::nullopt,
                        .diagnostic = {},
                        .system_error = std::nullopt,
                    },
                .revision = 1,
                .shutting_down = false,
            },
        .donbot =
            application::DonBotConfigurationSnapshot{
                .state = application::DonBotConfigurationState::Unverified,
                .api_base_url = std::string{config::default_donbot_api_base},
                .account_name = std::nullopt,
                .discord_summary_delivery_v1 = false,
                .guilds = {},
                .selected_guild_id = {},
                .diagnostic = {},
                .revision = 1,
                .shutting_down = false,
            },
        .twitch =
            application::TwitchConnectionSnapshot{
                .state = application::TwitchConnectionState::Disconnected,
                .login = std::nullopt,
                .user_code = std::nullopt,
                .verification_uri = std::nullopt,
                .authorization_expires_at = std::nullopt,
                .access_expires_at = std::nullopt,
                .diagnostic = {},
                .revision = 1,
                .shutting_down = false,
            },
        .twitch_test_message =
            application::TwitchTestMessageSnapshot{
                .state = application::TwitchTestMessageState::Idle,
                .diagnostic = {},
                .outcome = std::nullopt,
                .delivery_status = std::nullopt,
                .delivery_ambiguous = false,
                .revision = 1,
                .shutting_down = false,
            },
        .last_error = std::nullopt,
        .pending_commands = 0,
        .revision = 1,
        .accepting_commands = true,
        .shutting_down = false,
    };
}

void disconnected_model_tests(TestSuite& suite) {
    const auto model = ui::build_nexus_options_model(snapshot());
    MANNY_CHECK(suite, model.donbot.status_text == "Not verified");
    MANNY_CHECK(suite, model.donbot.verify_available);
    MANNY_CHECK(suite, !model.donbot.configuration_visible);
    MANNY_CHECK(suite, !model.donbot.discord_delivery_visible);
    MANNY_CHECK(suite, !model.donbot.guild_selection_available);
    MANNY_CHECK(suite, !model.donbot.enable_toggle_available);
    MANNY_CHECK(suite, !model.donbot.discord_delivery_toggle_available);
    MANNY_CHECK(suite, !model.donbot.discord_channel_selection_available);
    MANNY_CHECK(suite, !model.donbot.disconnect_available);
    MANNY_CHECK(suite, model.twitch.status_text == "Not connected");
    MANNY_CHECK(suite, model.twitch.connect_available);
    MANNY_CHECK(suite, !model.twitch.enable_toggle_available);
    MANNY_CHECK(suite, !model.twitch.disconnect_available);
    MANNY_CHECK(suite, model.twitch.test_message_status_text == "No test message sent");
    MANNY_CHECK(suite, !model.twitch.test_message_available);
    MANNY_CHECK(suite, model.settings_editable);
    MANNY_CHECK(suite, model.save_available);
    MANNY_CHECK(suite, !model.command_pending);
    MANNY_CHECK(suite, model.ordinary.general.log_directory == "C:/logs");
    MANNY_CHECK(suite, model.protected_storage_text == "Protected credential storage is available");
}

void connected_model_tests(TestSuite& suite) {
    auto state = snapshot();
    state.pending_commands = 2;
    state.donbot.state = application::DonBotConfigurationState::Verified;
    state.donbot.account_name = "Player.1234";
    state.donbot.discord_summary_delivery_v1 = true;
    state.donbot.guilds = {{
        .guild_id = "123",
        .guild_name = "Guild One",
        .discord_delivery =
            ports::DonBotDiscordDeliveryPolicy{
                .enabled = true,
                .defaults_available = true,
                .channel_override_allowed = true,
                .channels = {{.channel_id = "223", .channel_name = "logs"}},
            },
    }};
    state.donbot.selected_guild_id = "123";
    state.configuration.settings.donbot.enabled = true;
    state.configuration.settings.donbot.selected_guild_id = "123";
    state.twitch.state = application::TwitchConnectionState::Connected;
    state.twitch.login = "broadcaster_name";
    state.last_error = application::NexusOptionsError{
        .code = application::NexusOptionsErrorCode::ActionFailed,
        .message = "Safe visible diagnostic",
        .settings_validation_errors = {},
        .configuration_error = std::nullopt,
        .donbot_error = std::nullopt,
        .twitch_error = std::nullopt,
        .twitch_test_message_error = std::nullopt,
    };

    const auto model = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, model.donbot.status_text == "Verified as Player.1234");
    MANNY_CHECK(suite, model.donbot.configuration_visible);
    MANNY_CHECK(suite, model.donbot.guild_selection_available);
    MANNY_CHECK(suite, model.donbot.enable_toggle_available);
    MANNY_CHECK(suite, model.donbot.discord_delivery_visible);
    MANNY_CHECK(suite, model.donbot.discord_delivery_toggle_available);
    MANNY_CHECK(suite, model.donbot.discord_channel_selection_visible);
    MANNY_CHECK(suite, model.donbot.discord_channel_selection_available);
    MANNY_CHECK(suite, model.donbot.disconnect_available);
    MANNY_CHECK(suite, model.twitch.status_text == "Connected as broadcaster_name");
    MANNY_CHECK(suite, !model.twitch.connect_available);
    MANNY_CHECK(suite, model.twitch.enable_toggle_available);
    MANNY_CHECK(suite, model.twitch.disconnect_available);
    MANNY_CHECK(suite, model.twitch.test_message_available);
    MANNY_CHECK(suite, model.command_pending);
    MANNY_CHECK(suite, model.last_error == "Safe visible diagnostic");

    state.configuration.settings.donbot.discord_delivery_enabled = true;
    const auto delivery = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, delivery.donbot.discord_delivery_toggle_available);
    MANNY_CHECK(suite, delivery.donbot.discord_channel_selection_visible);
    MANNY_CHECK(suite, delivery.donbot.discord_channel_selection_available);

    state.configuration.settings.donbot.discord_delivery_enabled = false;
    state.donbot.guilds.front().discord_delivery.defaults_available = false;
    const auto override_only = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, override_only.donbot.discord_delivery_visible);
    MANNY_CHECK(suite, override_only.donbot.discord_channel_selection_visible);
    MANNY_CHECK(suite, override_only.donbot.discord_channel_selection_available);
    state.donbot.guilds.front().discord_delivery.defaults_available = true;

    state.configuration.settings.donbot.enabled = false;
    const auto donbot_disabled = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, !donbot_disabled.donbot.configuration_visible);
    MANNY_CHECK(suite, !donbot_disabled.donbot.discord_delivery_visible);
    state.configuration.settings.donbot.enabled = true;

    state.donbot.guilds.front().discord_delivery.enabled = false;
    const auto discord_disabled = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, !discord_disabled.donbot.discord_delivery_visible);
    MANNY_CHECK(suite, discord_disabled.donbot.discord_delivery_status_text ==
                           "Discord summaries are disabled in this server's DonBot settings");
    state.donbot.guilds.front().discord_delivery.enabled = true;

    state.twitch_test_message.state = application::TwitchTestMessageState::Sending;
    state.twitch_test_message.diagnostic = "Sending a Twitch test message";
    const auto sending = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, sending.twitch.test_message_status_text == "Sending test message");
    MANNY_CHECK(suite, sending.twitch.test_message_diagnostic == "Sending a Twitch test message");
    MANNY_CHECK(suite, !sending.twitch.test_message_available);

    state.twitch_test_message.state = application::TwitchTestMessageState::Error;
    state.twitch_test_message.diagnostic = "Twitch held the message for moderation";
    const auto failed = ui::build_nexus_options_model(state);
    MANNY_CHECK(suite, failed.twitch.test_message_status_text == "Test message error");
    MANNY_CHECK(suite, failed.twitch.test_message_available);
}

void authorization_and_disabled_model_tests(TestSuite& suite) {
    auto authorizing = snapshot();
    authorizing.twitch.state = application::TwitchConnectionState::AwaitingUser;
    authorizing.twitch.user_code = "ABCD-EFGH";
    authorizing.twitch.verification_uri = "https://www.twitch.tv/activate";
    auto model = ui::build_nexus_options_model(authorizing);
    MANNY_CHECK(suite, model.twitch.status_text == "Waiting for Twitch authorization");
    MANNY_CHECK(suite, model.twitch.user_code == "ABCD-EFGH");
    MANNY_CHECK(suite, model.twitch.verification_uri == "https://www.twitch.tv/activate");
    MANNY_CHECK(suite, !model.twitch.connect_available);
    MANNY_CHECK(suite, model.twitch.disconnect_available);

    auto unavailable = snapshot();
    unavailable.configuration.persistent_secret_storage.state =
        application::PersistentSecretStorageState::UnsupportedEnvironment;
    model = ui::build_nexus_options_model(unavailable);
    MANNY_CHECK(suite, !model.donbot.verify_available);
    MANNY_CHECK(suite, !model.twitch.connect_available);
    MANNY_CHECK(suite, model.protected_storage_text ==
                           "Protected credential storage is unavailable in this environment");

    auto stopping = snapshot();
    stopping.accepting_commands = false;
    stopping.shutting_down = true;
    model = ui::build_nexus_options_model(stopping);
    MANNY_CHECK(suite, !model.settings_editable);
    MANNY_CHECK(suite, !model.save_available);
    MANNY_CHECK(suite, !model.donbot.verify_available);
    MANNY_CHECK(suite, !model.twitch.connect_available);
}

} // namespace

void run_nexus_options_model_tests(TestSuite& suite) {
    MANNY_CHECK(suite, ui::escape_imgui_label_text("Guild ## Logs") == "Guild # # Logs");
    MANNY_CHECK(suite, ui::escape_imgui_label_text("Guild") == "Guild");
    disconnected_model_tests(suite);
    connected_model_tests(suite);
    authorization_and_disabled_model_tests(suite);
}

} // namespace manny_uploader::test
