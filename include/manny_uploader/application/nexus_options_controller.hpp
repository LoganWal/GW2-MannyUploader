#pragma once

#include "manny_uploader/application/configuration_service.hpp"
#include "manny_uploader/application/donbot_configuration_controller.hpp"
#include "manny_uploader/application/twitch_authentication_controller.hpp"
#include "manny_uploader/ports/twitch_test_messenger.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace manny_uploader::application {

struct NexusOrdinaryOptions {
    config::GeneralSettings general;
    config::DpsReportSettings dps_report;
    config::WingmanSettings wingman;
    std::string twitch_message_template;
    bool twitch_post_success{true};
    bool twitch_post_failure{true};
};

[[nodiscard]] NexusOrdinaryOptions ordinary_options_from(const config::Settings& settings);

struct SaveOrdinaryOptionsCommand {
    NexusOrdinaryOptions options;
};

struct SetWindowVisibleCommand {
    bool visible;
};

struct VerifyDonBotCommand {
    std::string api_base_url;
    support::SecretValue api_key;
};

struct SelectDonBotGuildCommand {
    std::string guild_id;
};

struct SetDonBotEnabledCommand {
    bool enabled;
};

struct DisconnectDonBotCommand {};

struct ConnectTwitchCommand {};

struct SetTwitchEnabledCommand {
    bool enabled;
};

struct DisconnectTwitchCommand {};

struct SendTwitchTestMessageCommand {};

struct DismissNexusOptionsErrorCommand {};

using NexusOptionsCommand =
    std::variant<SaveOrdinaryOptionsCommand, SetWindowVisibleCommand, VerifyDonBotCommand,
                 SelectDonBotGuildCommand, SetDonBotEnabledCommand, DisconnectDonBotCommand,
                 ConnectTwitchCommand, SetTwitchEnabledCommand, DisconnectTwitchCommand,
                 SendTwitchTestMessageCommand, DismissNexusOptionsErrorCommand>;

enum class TwitchTestMessageState : std::uint8_t {
    Idle,
    Sending,
    Sent,
    Error,
    ShuttingDown,
};

struct TwitchTestMessageSnapshot {
    TwitchTestMessageState state;
    std::string diagnostic;
    std::optional<ports::TwitchTestMessageOutcome> outcome;
    std::optional<domain::TwitchDeliveryStatus> delivery_status;
    bool delivery_ambiguous{};
    std::uint64_t revision{};
    bool shutting_down{};
};

enum class NexusOptionsErrorCode : std::uint8_t {
    InvalidConfiguration,
    InvalidCommand,
    QueueFull,
    Busy,
    ActionFailed,
    ShuttingDown,
};

struct NexusOptionsError {
    NexusOptionsErrorCode code;
    std::string message;
    std::vector<config::SettingsValidationError> settings_validation_errors;
    std::optional<ConfigurationErrorCode> configuration_error;
    std::optional<DonBotConfigurationErrorCode> donbot_error;
    std::optional<TwitchAuthenticationControllerErrorCode> twitch_error;
    std::optional<ports::TwitchTestMessageOutcome> twitch_test_message_error;
};

struct NexusOptionsSnapshot {
    ConfigurationSnapshot configuration;
    DonBotConfigurationSnapshot donbot;
    TwitchConnectionSnapshot twitch;
    TwitchTestMessageSnapshot twitch_test_message;
    std::optional<NexusOptionsError> last_error;
    std::size_t pending_commands{};
    std::uint64_t revision{};
    bool accepting_commands{};
    bool shutting_down{};
};

struct NexusOptionsTickReport {
    std::size_t commands_processed{};
    std::size_t action_failures{};
    bool donbot_progressed{};
    bool twitch_progressed{};
    bool twitch_test_message_progressed{};
};

struct NexusOptionsControllerConfig {
    std::size_t command_capacity{32};
    std::size_t max_commands_per_tick{8};
};

class NexusOptionsController {
  public:
    [[nodiscard]] static std::expected<NexusOptionsController, NexusOptionsError>
    create(ConfigurationService& configuration, DonBotConfigurationController& donbot,
           TwitchAuthenticationController& twitch,
           ports::ITwitchTestMessenger& twitch_test_messenger,
           NexusOptionsControllerConfig config = {});

    ~NexusOptionsController();
    NexusOptionsController(NexusOptionsController&&) noexcept;
    NexusOptionsController& operator=(NexusOptionsController&&) noexcept;
    NexusOptionsController(const NexusOptionsController&) = delete;
    NexusOptionsController& operator=(const NexusOptionsController&) = delete;

    [[nodiscard]] std::expected<void, NexusOptionsError> submit(NexusOptionsCommand command);
    [[nodiscard]] std::expected<NexusOptionsTickReport, NexusOptionsError> tick();
    [[nodiscard]] NexusOptionsSnapshot snapshot() const;

    void shutdown() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    struct State;

    explicit NexusOptionsController(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;
};

} // namespace manny_uploader::application
