#include "manny_uploader/application/nexus_options_controller.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <ranges>
#include <string_view>
#include <utility>

namespace manny_uploader::application {
namespace {

constexpr std::size_t max_command_capacity = 256;

[[nodiscard]] NexusOptionsError make_error(NexusOptionsErrorCode code, std::string message) {
    return NexusOptionsError{
        .code = code,
        .message = std::move(message),
        .settings_validation_errors = {},
        .configuration_error = std::nullopt,
        .donbot_error = std::nullopt,
        .twitch_error = std::nullopt,
        .twitch_test_message_error = std::nullopt,
    };
}

[[nodiscard]] NexusOptionsError from_configuration_error(const ConfigurationError& error) {
    auto mapped = make_error(NexusOptionsErrorCode::ActionFailed, error.message);
    mapped.settings_validation_errors = error.settings_validation_errors;
    mapped.configuration_error = error.code;
    return mapped;
}

[[nodiscard]] NexusOptionsError from_donbot_error(const DonBotConfigurationError& error) {
    auto mapped = make_error(NexusOptionsErrorCode::ActionFailed, error.message);
    mapped.configuration_error = error.configuration_error;
    mapped.donbot_error = error.code;
    return mapped;
}

[[nodiscard]] NexusOptionsError
from_twitch_error(const TwitchAuthenticationControllerError& error) {
    auto mapped = make_error(NexusOptionsErrorCode::ActionFailed, error.message);
    mapped.configuration_error = error.configuration_error;
    mapped.twitch_error = error.code;
    return mapped;
}

[[nodiscard]] NexusOptionsError
from_twitch_test_message_result(const ports::TwitchTestMessageResult& result) {
    auto mapped = make_error(NexusOptionsErrorCode::ActionFailed, result.detail);
    mapped.twitch_test_message_error = result.outcome;
    return mapped;
}

[[nodiscard]] config::Settings apply_ordinary_options(config::Settings settings,
                                                      const NexusOrdinaryOptions& options) {
    settings.general = options.general;
    settings.dps_report = options.dps_report;
    settings.wingman = options.wingman;
    settings.twitch.message_template = options.twitch_message_template;
    settings.twitch.post_success = options.twitch_post_success;
    settings.twitch.post_failure = options.twitch_post_failure;
    return settings;
}

[[nodiscard]] bool contains_guild(const DonBotConfigurationSnapshot& donbot,
                                  std::string_view guild_id) noexcept {
    return std::ranges::any_of(donbot.guilds, [guild_id](const ports::DonBotGuild& guild) {
        return guild.guild_id == guild_id;
    });
}

[[nodiscard]] std::expected<void, NexusOptionsError>
validate_submit_command(const NexusOptionsCommand& command,
                        const ConfigurationSnapshot& configuration) {
    if (const auto* save = std::get_if<SaveOrdinaryOptionsCommand>(&command)) {
        auto candidate = apply_ordinary_options(configuration.settings, save->options);
        auto errors = config::validate_settings(candidate);
        if (!errors.empty()) {
            auto result = make_error(NexusOptionsErrorCode::InvalidCommand,
                                     "The options contain invalid settings");
            result.settings_validation_errors = std::move(errors);
            return std::unexpected(std::move(result));
        }
    }
    if (const auto* verify = std::get_if<VerifyDonBotCommand>(&command)) {
        auto candidate = configuration.settings;
        candidate.donbot.enabled = false;
        candidate.donbot.selected_guild_id.clear();
        candidate.donbot.api_base_url = verify->api_base_url;
        auto errors = config::validate_settings(candidate);
        if (verify->api_key.empty() || !errors.empty()) {
            auto result = make_error(NexusOptionsErrorCode::InvalidCommand,
                                     "The DonBot endpoint or API key is invalid");
            result.settings_validation_errors = std::move(errors);
            return std::unexpected(std::move(result));
        }
    }
    return {};
}

} // namespace

struct NexusOptionsController::State {
    State(ConfigurationService& configuration_value, DonBotConfigurationController& donbot_value,
          TwitchAuthenticationController& twitch_value,
          ports::ITwitchTestMessenger& twitch_test_messenger_value,
          NexusOptionsControllerConfig config_value, NexusOptionsSnapshot published_value)
        : configuration{configuration_value}, donbot{donbot_value}, twitch{twitch_value},
          twitch_test_messenger{twitch_test_messenger_value}, config{config_value},
          twitch_test_message{published_value.twitch_test_message},
          published{std::move(published_value)} {}

    ConfigurationService& configuration;
    DonBotConfigurationController& donbot;
    TwitchAuthenticationController& twitch;
    ports::ITwitchTestMessenger& twitch_test_messenger;
    NexusOptionsControllerConfig config;
    mutable std::mutex mutex;
    std::deque<NexusOptionsCommand> commands;
    TwitchTestMessageSnapshot twitch_test_message;
    std::optional<std::uint64_t> twitch_test_message_in_flight;
    std::uint64_t next_twitch_test_message_id{1};
    NexusOptionsSnapshot published;
    bool ticking{};

    [[nodiscard]] std::expected<std::deque<NexusOptionsCommand>, NexusOptionsError> take_commands();
    [[nodiscard]] std::expected<void, NexusOptionsError> save_settings(config::Settings settings);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(SaveOrdinaryOptionsCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError> execute(VerifyDonBotCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError> execute(SelectDonBotGuildCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(const SetDonBotEnabledCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(const DisconnectDonBotCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(const ConnectTwitchCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(const SetTwitchEnabledCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(const DisconnectTwitchCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute(const SendTwitchTestMessageCommand& command);
    [[nodiscard]] static std::expected<void, NexusOptionsError>
    execute(const DismissNexusOptionsErrorCommand& command);
    [[nodiscard]] std::expected<void, NexusOptionsError>
    execute_command(NexusOptionsCommand& command);
    [[nodiscard]] std::expected<bool, NexusOptionsError> poll_twitch_test_message();
    void publish(std::optional<NexusOptionsError> last_error, bool error_dismissed);
};

std::expected<std::deque<NexusOptionsCommand>, NexusOptionsError>
NexusOptionsController::State::take_commands() {
    std::scoped_lock lock{mutex};
    if (published.shutting_down) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ShuttingDown, "Nexus options are shutting down"));
    }
    if (ticking) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::Busy, "A Nexus options tick is already running"));
    }
    ticking = true;

    std::deque<NexusOptionsCommand> pending;
    const auto count = std::min(config.max_commands_per_tick, commands.size());
    for (std::size_t index = 0; index < count; ++index) {
        pending.push_back(std::move(commands.front()));
        commands.pop_front();
    }
    published.pending_commands = commands.size();
    return pending;
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::save_settings(config::Settings settings) {
    if (auto saved = configuration.save_settings(std::move(settings)); !saved) {
        return std::unexpected(from_configuration_error(saved.error()));
    }
    return {};
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute(SaveOrdinaryOptionsCommand& command) {
    auto settings = apply_ordinary_options(configuration.snapshot().settings, command.options);
    return save_settings(std::move(settings));
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute(VerifyDonBotCommand& command) {
    auto started =
        donbot.begin_verification(std::move(command.api_base_url), std::move(command.api_key));
    if (!started) {
        return std::unexpected(from_donbot_error(started.error()));
    }
    return {};
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute(SelectDonBotGuildCommand& command) {
    auto selected = donbot.select_guild(std::move(command.guild_id));
    if (!selected) {
        return std::unexpected(from_donbot_error(selected.error()));
    }
    return {};
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute(const SetDonBotEnabledCommand& command) {
    auto settings = configuration.snapshot().settings;
    if (command.enabled) {
        const auto donbot_snapshot = donbot.snapshot();
        if (donbot_snapshot.state != DonBotConfigurationState::Verified ||
            !donbot_snapshot.account_name || donbot_snapshot.selected_guild_id.empty() ||
            donbot_snapshot.api_base_url != settings.donbot.api_base_url ||
            !contains_guild(donbot_snapshot, donbot_snapshot.selected_guild_id)) {
            return std::unexpected(
                make_error(NexusOptionsErrorCode::ActionFailed,
                           "Verify DonBot and select an authorized guild before enabling it"));
        }
    }
    settings.donbot.enabled = command.enabled;
    return save_settings(std::move(settings));
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute([[maybe_unused]] const DisconnectDonBotCommand& command) {
    auto disconnected = donbot.disconnect();
    if (!disconnected) {
        return std::unexpected(from_donbot_error(disconnected.error()));
    }
    return {};
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute([[maybe_unused]] const ConnectTwitchCommand& command) {
    auto started = twitch.begin_connection();
    if (!started) {
        return std::unexpected(from_twitch_error(started.error()));
    }
    return {};
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute(const SetTwitchEnabledCommand& command) {
    auto settings = configuration.snapshot().settings;
    if (command.enabled && twitch.snapshot().state != TwitchConnectionState::Connected) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ActionFailed,
                       "Connect the broadcaster's Twitch account before enabling chat"));
    }
    settings.twitch.enabled = command.enabled;
    return save_settings(std::move(settings));
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute([[maybe_unused]] const DisconnectTwitchCommand& command) {
    auto disconnected = twitch.disconnect();
    if (!disconnected) {
        return std::unexpected(from_twitch_error(disconnected.error()));
    }
    return {};
}

std::expected<void, NexusOptionsError> NexusOptionsController::State::execute(
    [[maybe_unused]] const SendTwitchTestMessageCommand& command) {
    if (twitch.snapshot().state != TwitchConnectionState::Connected) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ActionFailed,
                       "Connect the broadcaster's Twitch account before sending a test message"));
    }
    if (twitch_test_message_in_flight) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::Busy, "A Twitch test message is already in flight"));
    }
    if (next_twitch_test_message_id == 0) {
        return std::unexpected(make_error(NexusOptionsErrorCode::ActionFailed,
                                          "No more Twitch test messages can be created"));
    }

    const auto request_id = next_twitch_test_message_id;
    auto queued =
        twitch_test_messenger.enqueue(ports::TwitchTestMessageRequest{.request_id = request_id});
    if (!queued) {
        twitch_test_message.state = TwitchTestMessageState::Error;
        twitch_test_message.diagnostic = queued.error().message;
        twitch_test_message.outcome.reset();
        twitch_test_message.delivery_status.reset();
        twitch_test_message.delivery_ambiguous = false;
        ++twitch_test_message.revision;
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ActionFailed, queued.error().message));
    }

    ++next_twitch_test_message_id;
    twitch_test_message_in_flight = request_id;
    twitch_test_message.state = TwitchTestMessageState::Sending;
    twitch_test_message.diagnostic = "Sending a Twitch test message";
    twitch_test_message.outcome.reset();
    twitch_test_message.delivery_status.reset();
    twitch_test_message.delivery_ambiguous = false;
    ++twitch_test_message.revision;
    return {};
}

std::expected<void, NexusOptionsError> NexusOptionsController::State::execute(
    [[maybe_unused]] const DismissNexusOptionsErrorCommand& command) {
    return {};
}

std::expected<void, NexusOptionsError>
NexusOptionsController::State::execute_command(NexusOptionsCommand& command) {
    return std::visit([state = this](auto& value) { return state->execute(value); }, command);
}

std::expected<bool, NexusOptionsError> NexusOptionsController::State::poll_twitch_test_message() {
    auto result = twitch_test_messenger.try_take_result();
    if (!result) {
        return false;
    }
    if (!twitch_test_message_in_flight || result->request_id != *twitch_test_message_in_flight) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ActionFailed,
                       "Twitch test-message delivery returned an unexpected response"));
    }

    twitch_test_message_in_flight.reset();
    twitch_test_message.state = result->outcome == ports::TwitchTestMessageOutcome::Sent
                                    ? TwitchTestMessageState::Sent
                                    : TwitchTestMessageState::Error;
    twitch_test_message.diagnostic = result->detail;
    twitch_test_message.outcome = result->outcome;
    twitch_test_message.delivery_status = result->delivery_status;
    twitch_test_message.delivery_ambiguous = result->delivery_ambiguous;
    ++twitch_test_message.revision;
    if (result->outcome != ports::TwitchTestMessageOutcome::Sent) {
        return std::unexpected(from_twitch_test_message_result(*result));
    }
    return true;
}

void NexusOptionsController::State::publish(std::optional<NexusOptionsError> last_error,
                                            bool error_dismissed) {
    const auto configuration_snapshot = configuration.snapshot();
    const auto donbot_snapshot = donbot.snapshot();
    const auto twitch_snapshot = twitch.snapshot();
    std::scoped_lock lock{mutex};
    published.configuration = configuration_snapshot;
    published.donbot = donbot_snapshot;
    published.twitch = twitch_snapshot;
    if (!published.shutting_down) {
        published.twitch_test_message = twitch_test_message;
    }
    if (last_error || error_dismissed) {
        published.last_error = std::move(last_error);
    }
    published.pending_commands = commands.size();
    ticking = false;
    ++published.revision;
}

NexusOrdinaryOptions ordinary_options_from(const config::Settings& settings) {
    return NexusOrdinaryOptions{
        .general = settings.general,
        .dps_report = settings.dps_report,
        .wingman = settings.wingman,
        .twitch_message_template = settings.twitch.message_template,
        .twitch_post_success = settings.twitch.post_success,
        .twitch_post_failure = settings.twitch.post_failure,
    };
}

std::expected<NexusOptionsController, NexusOptionsError> NexusOptionsController::create(
    ConfigurationService& configuration, DonBotConfigurationController& donbot,
    TwitchAuthenticationController& twitch, ports::ITwitchTestMessenger& twitch_test_messenger,
    NexusOptionsControllerConfig config) {
    if (config.command_capacity == 0 || config.command_capacity > max_command_capacity ||
        config.max_commands_per_tick == 0 ||
        config.max_commands_per_tick > config.command_capacity) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::InvalidConfiguration,
                       "Nexus options queue limits are outside the supported range"));
    }
    const auto configuration_snapshot = configuration.snapshot();
    const auto donbot_snapshot = donbot.snapshot();
    const auto twitch_snapshot = twitch.snapshot();
    if (configuration_snapshot.shutting_down || donbot_snapshot.shutting_down ||
        twitch_snapshot.shutting_down) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ShuttingDown, "Nexus options are shutting down"));
    }

    auto state =
        std::make_unique<State>(configuration, donbot, twitch, twitch_test_messenger, config,
                                NexusOptionsSnapshot{
                                    .configuration = configuration_snapshot,
                                    .donbot = donbot_snapshot,
                                    .twitch = twitch_snapshot,
                                    .twitch_test_message =
                                        TwitchTestMessageSnapshot{
                                            .state = TwitchTestMessageState::Idle,
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
                                });
    return NexusOptionsController{std::move(state)};
}

NexusOptionsController::NexusOptionsController(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

NexusOptionsController::~NexusOptionsController() = default;
NexusOptionsController::NexusOptionsController(NexusOptionsController&&) noexcept = default;
NexusOptionsController&
NexusOptionsController::operator=(NexusOptionsController&&) noexcept = default;

std::expected<void, NexusOptionsError> NexusOptionsController::submit(NexusOptionsCommand command) {
    std::scoped_lock lock{state_->mutex};
    if (state_->published.shutting_down || !state_->published.accepting_commands) {
        return std::unexpected(
            make_error(NexusOptionsErrorCode::ShuttingDown, "Nexus options are shutting down"));
    }
    if (auto valid = validate_submit_command(command, state_->published.configuration); !valid) {
        auto error = std::move(valid.error());
        state_->published.last_error = error;
        ++state_->published.revision;
        return std::unexpected(std::move(error));
    }
    if (state_->commands.size() >= state_->config.command_capacity) {
        auto error =
            make_error(NexusOptionsErrorCode::QueueFull, "The Nexus options command queue is full");
        state_->published.last_error = error;
        ++state_->published.revision;
        return std::unexpected(std::move(error));
    }
    state_->commands.push_back(std::move(command));
    state_->published.pending_commands = state_->commands.size();
    ++state_->published.revision;
    return {};
}

std::expected<NexusOptionsTickReport, NexusOptionsError> NexusOptionsController::tick() {
    auto commands = state_->take_commands();
    if (!commands) {
        return std::unexpected(std::move(commands.error()));
    }

    NexusOptionsTickReport report;
    std::optional<NexusOptionsError> last_error;
    const auto record = [&report, &last_error](NexusOptionsError error) {
        ++report.action_failures;
        last_error = std::move(error);
    };
    bool error_dismissed{};
    for (auto& command : *commands) {
        ++report.commands_processed;
        auto handled = state_->execute_command(command);
        if (!handled) {
            record(std::move(handled.error()));
        }
        if (std::holds_alternative<DismissNexusOptionsErrorCommand>(command)) {
            last_error.reset();
            error_dismissed = true;
        }
    }

    if (auto progressed = state_->donbot.poll(); progressed) {
        report.donbot_progressed = *progressed;
    } else {
        record(from_donbot_error(progressed.error()));
    }
    if (auto progressed = state_->twitch.tick(); progressed) {
        report.twitch_progressed = *progressed;
    } else {
        record(from_twitch_error(progressed.error()));
    }
    if (auto progressed = state_->poll_twitch_test_message(); progressed) {
        report.twitch_test_message_progressed = *progressed;
    } else {
        record(std::move(progressed.error()));
    }

    state_->publish(std::move(last_error), error_dismissed);
    return report;
}

NexusOptionsSnapshot NexusOptionsController::snapshot() const {
    std::scoped_lock lock{state_->mutex};
    return state_->published;
}

void NexusOptionsController::shutdown() noexcept {
    {
        std::scoped_lock lock{state_->mutex};
        if (state_->published.shutting_down) {
            return;
        }
        state_->published.accepting_commands = false;
        state_->published.shutting_down = true;
        state_->published.twitch_test_message.state = TwitchTestMessageState::ShuttingDown;
        state_->published.twitch_test_message.diagnostic = "Shutting down";
        state_->published.twitch_test_message.shutting_down = true;
        ++state_->published.twitch_test_message.revision;
        state_->commands.clear();
        state_->published.pending_commands = 0;
        ++state_->published.revision;
    }
    state_->twitch_test_messenger.cancel_pending();
}

bool NexusOptionsController::is_shutting_down() const noexcept {
    std::scoped_lock lock{state_->mutex};
    return state_->published.shutting_down;
}

} // namespace manny_uploader::application
