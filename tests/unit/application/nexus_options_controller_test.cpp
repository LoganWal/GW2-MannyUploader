#include "manny_uploader/application/nexus_options_controller.hpp"
#include "manny_uploader/application/twitch_session_owner.hpp"
#include "manny_uploader/providers/twitch_client.hpp"

#include "support/test_suite.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] ports::SettingsStoreError settings_error() {
    return ports::SettingsStoreError{
        .code = ports::SettingsStoreErrorCode::FileWriteFailed,
        .message = "settings write failed",
        .path = "C:/addon/settings.json",
        .validation_errors = {},
    };
}

[[nodiscard]] ports::SecretStoreError secret_error(ports::SecretId id) {
    return ports::SecretStoreError{
        .code = ports::SecretStoreErrorCode::NotFound,
        .id = id,
        .message = "protected credential not found",
        .system_error = std::nullopt,
    };
}

class RecordingSettingsStore final : public ports::ISettingsStore {
  public:
    RecordingSettingsStore(config::Settings initial, std::vector<std::string>& events)
        : initial_{std::move(initial)}, events_{events} {}

    [[nodiscard]] std::expected<ports::SettingsLoadResult, ports::SettingsStoreError>
    load() const override {
        events_.push_back("settings.load");
        return ports::SettingsLoadResult{
            .settings = initial_,
            .source = ports::SettingsLoadSource::Primary,
            .recovery_diagnostic = std::nullopt,
        };
    }

    [[nodiscard]] std::expected<void, ports::SettingsStoreError>
    save(const config::Settings& settings) const override {
        events_.push_back("settings.save");
        if (fail_save) {
            return std::unexpected(settings_error());
        }
        saved.push_back(settings);
        return {};
    }

    config::Settings initial_;
    std::vector<std::string>& events_;
    mutable std::vector<config::Settings> saved;
    bool fail_save{};
};

class RecordingSecretStore final : public ports::ISecretStore {
  public:
    explicit RecordingSecretStore(std::vector<std::string>& events) : events_{events} {}

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        events_.push_back("secret.load");
        const auto found = values.find(id);
        if (found == values.end()) {
            return std::unexpected(secret_error(id));
        }
        return support::SecretValue{found->second};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId id, const support::SecretValue& value) override {
        events_.push_back("secret.store");
        values[id] = std::vector<std::byte>{value.bytes().begin(), value.bytes().end()};
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId id) override {
        events_.push_back("secret.erase");
        values.erase(id);
        return {};
    }

    std::vector<std::string>& events_;
    mutable std::map<ports::SecretId, std::vector<std::byte>> values;
};

class FakeDonBotVerifier final : public ports::IDonBotVerifier {
  public:
    explicit FakeDonBotVerifier(std::vector<std::string>& events) : events_{events} {}

    [[nodiscard]] std::expected<void, ports::DonBotVerificationDispatchError>
    enqueue(ports::DonBotVerificationRequest request) override {
        events_.push_back("donbot.enqueue");
        requests.push_back(std::move(request));
        return {};
    }

    [[nodiscard]] std::optional<ports::DonBotVerificationResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void cancel_pending() noexcept override {
        ++cancel_calls;
        requests.clear();
        results.clear();
    }

    void succeed_next() {
        auto request = std::move(requests.front());
        requests.pop_front();
        results.push_back(ports::DonBotVerificationResult{
            .request_id = request.request_id,
            .verification =
                ports::DonBotVerificationSuccess{
                    .identity =
                        ports::DonBotVerification{
                            .account_name = "Player.1234",
                            .guilds = {{.guild_id = "123", .guild_name = "Guild One"}},
                        },
                    .api_key = std::move(request.api_key),
                },
        });
    }

    std::vector<std::string>& events_;
    std::deque<ports::DonBotVerificationRequest> requests;
    std::deque<ports::DonBotVerificationResult> results;
    std::size_t cancel_calls{};
};

class FakeClock final : public ports::IClock {
  public:
    [[nodiscard]] std::chrono::system_clock::time_point system_now() const noexcept override {
        return system_now_;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const noexcept override {
        return steady_now_;
    }

    void advance(std::chrono::seconds duration) noexcept {
        system_now_ += duration;
        steady_now_ += duration;
    }

    std::chrono::system_clock::time_point system_now_{
        std::chrono::system_clock::time_point{1'800'000'000s}};
    std::chrono::steady_clock::time_point steady_now_{std::chrono::steady_clock::time_point{100s}};
};

class UnusedTwitchClient final : public providers::ITwitchClient {
  public:
    [[nodiscard]] std::expected<providers::TwitchDeviceAuthorization, providers::TwitchError>
    start_device_authorization(const std::stop_token&) const override {
        return std::unexpected(error());
    }

    [[nodiscard]] std::expected<providers::TwitchDevicePollResult, providers::TwitchError>
    poll_device_authorization(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(error());
    }

    [[nodiscard]] std::expected<providers::TwitchValidatedIdentity, providers::TwitchError>
    validate_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(error());
    }

    [[nodiscard]] std::expected<providers::TwitchTokenGrant, providers::TwitchError>
    refresh_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(error());
    }

    [[nodiscard]] std::expected<void, providers::TwitchError>
    revoke_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(error());
    }

    [[nodiscard]] std::expected<providers::TwitchChatResult, providers::TwitchError>
    send_chat_message(std::string_view, std::string_view, const support::SecretValue&,
                      const std::stop_token&) const override {
        return std::unexpected(error());
    }

  private:
    [[nodiscard]] static providers::TwitchError error() {
        return providers::TwitchError{
            .disposition = providers::TwitchDisposition::Failed,
            .detail = "unused test client",
            .retry_after = std::nullopt,
            .http_error = std::nullopt,
            .http_status = std::nullopt,
        };
    }
};

class FakeTwitchAuthenticator final : public ports::ITwitchAuthenticator {
  public:
    explicit FakeTwitchAuthenticator(std::vector<std::string>& events) : events_{events} {}

    [[nodiscard]] std::expected<void, ports::TwitchAuthenticationDispatchError>
    enqueue(ports::TwitchAuthenticationRequest request) override {
        events_.push_back("twitch.enqueue");
        requests.push_back(std::move(request));
        return {};
    }

    [[nodiscard]] std::optional<ports::TwitchAuthenticationResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void cancel_pending() noexcept override {
        ++cancel_calls;
        requests.clear();
        results.clear();
    }

    void respond_start() {
        auto request = take(ports::TwitchAuthenticationOperation::Start);
        succeed(std::move(request),
                ports::TwitchAuthorizationStarted{
                    .device_code = support::SecretValue::from_text("PRIVATE-DEVICE"),
                    .user_code = "ABCD-EFGH",
                    .verification_uri = "https://www.twitch.tv/activate",
                    .expires_in = 600s,
                    .polling_interval = 5s,
                });
    }

    void respond_grant() {
        auto request = take(ports::TwitchAuthenticationOperation::Poll);
        succeed(std::move(request),
                ports::TwitchAuthorizationGranted{
                    .access_token = support::SecretValue::from_text("PRIVATE-ACCESS"),
                    .refresh_token = support::SecretValue::from_text("PRIVATE-REFRESH"),
                    .expires_in = 14'400s,
                    .scopes = {"user:write:chat"},
                });
    }

    void respond_validation() {
        auto request = take(ports::TwitchAuthenticationOperation::Validate);
        auto& validation = std::get<ports::TwitchValidateAuthentication>(request.command);
        auto response = ports::TwitchValidationSucceeded{
            .credentials = std::move(validation.credentials),
            .user_id = "141981764",
            .login = "broadcaster_name",
            .expires_in = 14'400s,
            .scopes = {"user:write:chat"},
        };
        succeed(std::move(request), std::move(response));
    }

  private:
    template <typename Success>
    void succeed(ports::TwitchAuthenticationRequest request, Success success) {
        const auto operation = ports::authentication_operation(request.command);
        results.push_back(ports::TwitchAuthenticationResult{
            .request_id = request.request_id,
            .operation = operation,
            .outcome =
                ports::TwitchAuthenticationSuccess{std::in_place_type<Success>, std::move(success)},
        });
    }

    [[nodiscard]] ports::TwitchAuthenticationRequest
    take(ports::TwitchAuthenticationOperation expected) {
        auto request = std::move(requests.front());
        requests.pop_front();
        if (ports::authentication_operation(request.command) != expected) {
            unexpected_operation = true;
        }
        return request;
    }

  public:
    std::vector<std::string>& events_;
    std::deque<ports::TwitchAuthenticationRequest> requests;
    std::deque<ports::TwitchAuthenticationResult> results;
    std::size_t cancel_calls{};
    bool unexpected_operation{};
};

class FakeTwitchTestMessenger final : public ports::ITwitchTestMessenger {
  public:
    explicit FakeTwitchTestMessenger(std::vector<std::string>& events) : events_{events} {}

    [[nodiscard]] std::expected<void, ports::TwitchTestMessageDispatchError>
    enqueue(ports::TwitchTestMessageRequest request) override {
        events_.push_back("twitch_test.enqueue");
        if (fail_enqueue) {
            return std::unexpected(ports::TwitchTestMessageDispatchError{
                .message = "Twitch test-message queue is full"});
        }
        requests.push_back(request);
        return {};
    }

    [[nodiscard]] std::optional<ports::TwitchTestMessageResult> try_take_result() override {
        if (results.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }

    void cancel_pending() noexcept override {
        ++cancel_calls;
        requests.clear();
        results.clear();
    }

    void respond(ports::TwitchTestMessageOutcome outcome, std::string detail,
                 std::optional<domain::TwitchDeliveryStatus> delivery_status = std::nullopt,
                 bool delivery_ambiguous = false) {
        const auto request = requests.front();
        requests.pop_front();
        results.push_back(ports::TwitchTestMessageResult{
            .request_id = request.request_id,
            .outcome = outcome,
            .detail = std::move(detail),
            .retry_after = std::nullopt,
            .delivery_status = delivery_status,
            .delivery_ambiguous = delivery_ambiguous,
        });
    }

    std::vector<std::string>& events_;
    std::deque<ports::TwitchTestMessageRequest> requests;
    std::deque<ports::TwitchTestMessageResult> results;
    std::size_t cancel_calls{};
    bool fail_enqueue{};
};

struct Fixture {
    explicit Fixture(application::NexusOptionsControllerConfig controller_config = {})
        : donbot_verifier{events}, twitch_authenticator{events}, twitch_test_messenger{events} {
        auto settings_store = std::make_unique<RecordingSettingsStore>(
            config::make_default_settings("C:/logs"), events);
        settings_observer = settings_store.get();
        auto secret_store = std::make_unique<RecordingSecretStore>(events);
        secret_observer = secret_store.get();
        auto configuration_created = application::ConfigurationService::create(
            std::move(settings_store), std::move(secret_store));
        configuration.emplace(std::move(*configuration_created));
        auto donbot_created =
            application::DonBotConfigurationController::create(*configuration, donbot_verifier);
        donbot.emplace(std::move(*donbot_created));
        session_owner.emplace(*configuration, twitch_client, clock);
        auto twitch_created = application::TwitchAuthenticationController::create(
            *configuration, twitch_authenticator, *session_owner, clock);
        twitch.emplace(std::move(*twitch_created));
        auto options_created = application::NexusOptionsController::create(
            *configuration, *donbot, *twitch, twitch_test_messenger, controller_config);
        options.emplace(std::move(*options_created));
        events.clear();
    }

    std::vector<std::string> events;
    RecordingSettingsStore* settings_observer{};
    RecordingSecretStore* secret_observer{};
    std::optional<application::ConfigurationService> configuration;
    FakeDonBotVerifier donbot_verifier;
    std::optional<application::DonBotConfigurationController> donbot;
    UnusedTwitchClient twitch_client;
    FakeTwitchAuthenticator twitch_authenticator;
    FakeTwitchTestMessenger twitch_test_messenger;
    FakeClock clock;
    std::optional<application::TwitchSessionOwner> session_owner;
    std::optional<application::TwitchAuthenticationController> twitch;
    std::optional<application::NexusOptionsController> options;
};

[[nodiscard]] bool snapshot_contains(const application::NexusOptionsSnapshot& snapshot,
                                     std::string_view marker) {
    const auto contains = [marker](std::string_view value) {
        return value.find(marker) != std::string_view::npos;
    };
    const auto& settings = snapshot.configuration.settings;
    return contains(settings.general.log_directory) || contains(settings.donbot.api_base_url) ||
           contains(settings.donbot.selected_guild_id) ||
           contains(settings.twitch.message_template) || contains(snapshot.donbot.api_base_url) ||
           contains(snapshot.donbot.account_name.value_or("")) ||
           contains(snapshot.donbot.selected_guild_id) || contains(snapshot.donbot.diagnostic) ||
           contains(snapshot.twitch.login.value_or("")) ||
           contains(snapshot.twitch.user_code.value_or("")) ||
           contains(snapshot.twitch.verification_uri.value_or("")) ||
           contains(snapshot.twitch.diagnostic) ||
           contains(snapshot.twitch_test_message.diagnostic) ||
           (snapshot.last_error && contains(snapshot.last_error->message));
}

void connect_twitch(Fixture& fixture) {
    static_cast<void>(fixture.options->submit(application::ConnectTwitchCommand{}));
    static_cast<void>(fixture.options->tick());
    fixture.twitch_authenticator.respond_start();
    static_cast<void>(fixture.options->tick());
    fixture.clock.advance(5s);
    static_cast<void>(fixture.options->tick());
    fixture.twitch_authenticator.respond_grant();
    static_cast<void>(fixture.options->tick());
    fixture.twitch_authenticator.respond_validation();
    static_cast<void>(fixture.options->tick());
    fixture.events.clear();
}

void render_boundary_and_queue_tests(TestSuite& suite) {
    Fixture fixture{{.command_capacity = 2, .max_commands_per_tick = 1}};
    auto ordinary = application::ordinary_options_from(fixture.configuration->snapshot().settings);
    ordinary.wingman.enabled = false;
    ordinary.twitch_client_id = "abc123publicclient";
    ordinary.twitch_message_template = "{result}: {url}";

    MANNY_CHECK(suite, fixture.options
                           ->submit(application::SaveOrdinaryOptionsCommand{
                               .options = std::move(ordinary),
                           })
                           .has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.wingman.enabled);
    MANNY_CHECK(suite, fixture.options->snapshot().pending_commands == 1);

    MANNY_CHECK(suite, fixture.options->submit(application::ConnectTwitchCommand{}).has_value());
    const auto full =
        fixture.options->submit(application::SetTwitchEnabledCommand{.enabled = true});
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().code == application::NexusOptionsErrorCode::QueueFull);
    MANNY_CHECK(suite, fixture.events.empty());

    const auto first_tick = fixture.options->tick();
    MANNY_CHECK(suite, first_tick.has_value() && first_tick->commands_processed == 1);
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.wingman.enabled);
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.twitch.client_id ==
                           "abc123publicclient");
    MANNY_CHECK(suite, fixture.options->snapshot().pending_commands == 1);

    fixture.events.clear();
    const auto second_tick = fixture.options->tick();
    MANNY_CHECK(suite, second_tick.has_value() && second_tick->commands_processed == 1);
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"twitch.enqueue"}));
    MANNY_CHECK(suite, fixture.options->snapshot().twitch.state ==
                           application::TwitchConnectionState::Starting);

    auto invalid = application::ordinary_options_from(fixture.configuration->snapshot().settings);
    invalid.general.poll_interval_ms = 0;
    const auto rejected = fixture.options->submit(application::SaveOrdinaryOptionsCommand{
        .options = std::move(invalid),
    });
    MANNY_CHECK(suite, !rejected.has_value());
    MANNY_CHECK(suite, rejected.error().code == application::NexusOptionsErrorCode::InvalidCommand);
    MANNY_CHECK(suite, !rejected.error().settings_validation_errors.empty());
}

void workflow_owned_settings_tests(TestSuite& suite) {
    Fixture fixture;
    auto current = fixture.configuration->snapshot().settings;
    current.donbot.enabled = true;
    current.donbot.selected_guild_id = "123";
    current.twitch.enabled = true;
    MANNY_CHECK(suite, fixture.configuration->save_settings(current).has_value());
    fixture.events.clear();

    auto ordinary = application::ordinary_options_from(current);
    ordinary.general.recent_log_limit = 75;
    MANNY_CHECK(suite, fixture.options
                           ->submit(application::SaveOrdinaryOptionsCommand{
                               .options = std::move(ordinary),
                           })
                           .has_value());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    const auto saved = fixture.configuration->snapshot().settings;
    MANNY_CHECK(suite, saved.general.recent_log_limit == 75);
    MANNY_CHECK(suite, saved.donbot.enabled);
    MANNY_CHECK(suite, saved.donbot.selected_guild_id == "123");
    MANNY_CHECK(suite, saved.twitch.enabled);

    fixture.events.clear();
    MANNY_CHECK(suite,
                fixture.options->submit(application::SetWindowVisibleCommand{.visible = false})
                    .has_value());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    const auto hidden = fixture.configuration->snapshot().settings;
    MANNY_CHECK(suite, !hidden.general.window_visible);
    MANNY_CHECK(suite, hidden.general.recent_log_limit == 75);
    MANNY_CHECK(suite, hidden.donbot.enabled);
    MANNY_CHECK(suite, hidden.twitch.enabled);
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
}

void concurrent_submission_tests(TestSuite& suite) {
    constexpr std::size_t thread_count = 4;
    constexpr std::size_t commands_per_thread = 16;
    Fixture fixture{{.command_capacity = thread_count * commands_per_thread,
                     .max_commands_per_tick = thread_count * commands_per_thread}};
    std::atomic<std::size_t> failures{};
    {
        std::vector<std::jthread> submitters;
        for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
            submitters.emplace_back([&fixture, &failures] {
                for (std::size_t command_index = 0; command_index < commands_per_thread;
                     ++command_index) {
                    if (!fixture.options->submit(application::DismissNexusOptionsErrorCommand{})
                             .has_value()) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    static_cast<void>(fixture.options->snapshot());
                }
            });
        }
    }
    MANNY_CHECK(suite, failures.load(std::memory_order_relaxed) == 0);
    MANNY_CHECK(suite,
                fixture.options->snapshot().pending_commands == thread_count * commands_per_thread);
    MANNY_CHECK(suite, fixture.events.empty());
    const auto drained = fixture.options->tick();
    MANNY_CHECK(suite, drained.has_value());
    MANNY_CHECK(suite, drained->commands_processed == thread_count * commands_per_thread);
    MANNY_CHECK(suite, fixture.options->snapshot().pending_commands == 0);
}

void donbot_workflow_tests(TestSuite& suite) {
    constexpr std::string_view marker = "PRIVATE-DONBOT-KEY";
    Fixture fixture;
    MANNY_CHECK(suite, fixture.options
                           ->submit(application::VerifyDonBotCommand{
                               .api_base_url = "https://donbot.example/",
                               .api_key = support::SecretValue::from_text(marker),
                           })
                           .has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.donbot_verifier.requests.empty());
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), marker));

    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"donbot.enqueue"}));
    MANNY_CHECK(suite, fixture.donbot_verifier.requests.size() == 1);
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), marker));

    fixture.donbot_verifier.succeed_next();
    fixture.events.clear();
    const auto verified = fixture.options->tick();
    MANNY_CHECK(suite, verified.has_value() && verified->donbot_progressed);
    MANNY_CHECK(suite,
                fixture.events == std::vector<std::string>({"settings.save", "secret.store"}));
    MANNY_CHECK(suite, fixture.options->snapshot().donbot.state ==
                           application::DonBotConfigurationState::Verified);
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), marker));

    fixture.events.clear();
    MANNY_CHECK(
        suite,
        fixture.options->submit(application::SetDonBotEnabledCommand{.enabled = true}).has_value());
    MANNY_CHECK(suite, fixture.options->tick()->action_failures == 1);
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.enabled);

    MANNY_CHECK(suite,
                fixture.options->submit(application::SelectDonBotGuildCommand{.guild_id = "123"})
                    .has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite,
                fixture.configuration->snapshot().settings.donbot.selected_guild_id == "123");
    fixture.events.clear();
    MANNY_CHECK(
        suite,
        fixture.options->submit(application::SetDonBotEnabledCommand{.enabled = true}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.donbot.enabled);

    fixture.events.clear();
    MANNY_CHECK(suite, fixture.options->submit(application::DisconnectDonBotCommand{}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite,
                fixture.events == std::vector<std::string>({"settings.save", "secret.erase"}));
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.donbot.enabled);
}

void twitch_workflow_tests(TestSuite& suite) {
    Fixture fixture;
    MANNY_CHECK(
        suite,
        fixture.options->submit(application::SetTwitchEnabledCommand{.enabled = true}).has_value());
    const auto premature = fixture.options->tick();
    MANNY_CHECK(suite, premature.has_value() && premature->action_failures == 1);
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->snapshot().last_error.has_value());
    MANNY_CHECK(
        suite, fixture.options->submit(application::DismissNexusOptionsErrorCommand{}).has_value());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, !fixture.options->snapshot().last_error.has_value());

    MANNY_CHECK(suite, fixture.options->submit(application::ConnectTwitchCommand{}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"twitch.enqueue"}));
    fixture.twitch_authenticator.respond_start();
    fixture.events.clear();
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.options->snapshot().twitch.user_code == "ABCD-EFGH");
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), "PRIVATE-DEVICE"));

    fixture.clock.advance(5s);
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    fixture.twitch_authenticator.respond_grant();
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    fixture.twitch_authenticator.respond_validation();
    fixture.events.clear();
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"secret.store"}));
    MANNY_CHECK(suite, fixture.options->snapshot().twitch.state ==
                           application::TwitchConnectionState::Connected);
    MANNY_CHECK(suite, fixture.options->snapshot().twitch.login == "broadcaster_name");
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), "PRIVATE-ACCESS"));
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), "PRIVATE-REFRESH"));

    auto changed_client_id =
        application::ordinary_options_from(fixture.configuration->snapshot().settings);
    changed_client_id.twitch_client_id = "different123client";
    MANNY_CHECK(suite, fixture.options
                           ->submit(application::SaveOrdinaryOptionsCommand{
                               .options = std::move(changed_client_id),
                           })
                           .has_value());
    const auto rejected_client_change = fixture.options->tick();
    MANNY_CHECK(suite, rejected_client_change && rejected_client_change->action_failures == 1);
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.twitch.client_id.empty());

    fixture.events.clear();
    MANNY_CHECK(
        suite,
        fixture.options->submit(application::SetTwitchEnabledCommand{.enabled = true}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"settings.save"}));
    MANNY_CHECK(suite, fixture.configuration->snapshot().settings.twitch.enabled);

    fixture.events.clear();
    MANNY_CHECK(suite, fixture.options->submit(application::DisconnectTwitchCommand{}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite,
                fixture.events == std::vector<std::string>({"settings.save", "twitch.enqueue"}));
    MANNY_CHECK(suite, !fixture.configuration->snapshot().settings.twitch.enabled);
    MANNY_CHECK(suite, !fixture.twitch_authenticator.unexpected_operation);
}

void twitch_test_message_workflow_tests(TestSuite& suite) {
    Fixture fixture;
    MANNY_CHECK(suite,
                fixture.options->submit(application::SendTwitchTestMessageCommand{}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    const auto premature = fixture.options->tick();
    MANNY_CHECK(suite, premature && premature->action_failures == 1);
    MANNY_CHECK(suite, fixture.twitch_test_messenger.requests.empty());

    connect_twitch(fixture);
    MANNY_CHECK(suite, fixture.options->snapshot().twitch.state ==
                           application::TwitchConnectionState::Connected);
    MANNY_CHECK(suite, fixture.options->snapshot().twitch_test_message.state ==
                           application::TwitchTestMessageState::Idle);

    MANNY_CHECK(suite,
                fixture.options->submit(application::SendTwitchTestMessageCommand{}).has_value());
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.twitch_test_messenger.requests.empty());
    const auto started = fixture.options->tick();
    MANNY_CHECK(suite, started && started->commands_processed == 1);
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>{"twitch_test.enqueue"});
    MANNY_CHECK(suite, fixture.twitch_test_messenger.requests.size() == 1);
    MANNY_CHECK(suite, fixture.twitch_test_messenger.requests.front().request_id == 1);
    MANNY_CHECK(suite, fixture.options->snapshot().twitch_test_message.state ==
                           application::TwitchTestMessageState::Sending);

    fixture.events.clear();
    MANNY_CHECK(suite,
                fixture.options->submit(application::SendTwitchTestMessageCommand{}).has_value());
    const auto busy = fixture.options->tick();
    MANNY_CHECK(suite, busy && busy->action_failures == 1);
    MANNY_CHECK(suite, busy && busy->commands_processed == 1);
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.twitch_test_messenger.requests.size() == 1);
    MANNY_CHECK(suite, fixture.options->snapshot().last_error->code ==
                           application::NexusOptionsErrorCode::Busy);

    fixture.twitch_test_messenger.respond(ports::TwitchTestMessageOutcome::Sent,
                                          "Twitch test message sent",
                                          domain::TwitchDeliveryStatus::Sent);
    const auto delivered = fixture.options->tick();
    MANNY_CHECK(suite, delivered && delivered->twitch_test_message_progressed);
    const auto sent_snapshot = fixture.options->snapshot().twitch_test_message;
    MANNY_CHECK(suite, sent_snapshot.state == application::TwitchTestMessageState::Sent);
    MANNY_CHECK(suite, sent_snapshot.outcome == ports::TwitchTestMessageOutcome::Sent);
    MANNY_CHECK(suite, sent_snapshot.delivery_status == domain::TwitchDeliveryStatus::Sent);
    MANNY_CHECK(suite, !sent_snapshot.delivery_ambiguous);

    fixture.events.clear();
    MANNY_CHECK(suite,
                fixture.options->submit(application::SendTwitchTestMessageCommand{}).has_value());
    MANNY_CHECK(suite, fixture.options->tick().has_value());
    MANNY_CHECK(suite, fixture.twitch_test_messenger.requests.front().request_id == 2);
    fixture.twitch_test_messenger.results.push_back(ports::TwitchTestMessageResult{
        .request_id = 999,
        .outcome = ports::TwitchTestMessageOutcome::Sent,
        .detail = "stale response",
        .retry_after = std::nullopt,
        .delivery_status = domain::TwitchDeliveryStatus::Sent,
        .delivery_ambiguous = false,
    });
    const auto stale = fixture.options->tick();
    MANNY_CHECK(suite, stale && stale->action_failures == 1);
    MANNY_CHECK(suite, fixture.options->snapshot().twitch_test_message.state ==
                           application::TwitchTestMessageState::Sending);
    fixture.twitch_test_messenger.respond(ports::TwitchTestMessageOutcome::Failed,
                                          "Delivery confirmation is unavailable", std::nullopt,
                                          true);
    const auto ambiguous = fixture.options->tick();
    MANNY_CHECK(suite, ambiguous && ambiguous->action_failures == 1);
    const auto failed_snapshot = fixture.options->snapshot().twitch_test_message;
    MANNY_CHECK(suite, failed_snapshot.state == application::TwitchTestMessageState::Error);
    MANNY_CHECK(suite, failed_snapshot.outcome == ports::TwitchTestMessageOutcome::Failed);
    MANNY_CHECK(suite, failed_snapshot.delivery_ambiguous);
    MANNY_CHECK(suite, fixture.options->snapshot().last_error->twitch_test_message_error ==
                           ports::TwitchTestMessageOutcome::Failed);

    fixture.twitch_test_messenger.fail_enqueue = true;
    fixture.events.clear();
    MANNY_CHECK(suite,
                fixture.options->submit(application::SendTwitchTestMessageCommand{}).has_value());
    const auto rejected = fixture.options->tick();
    MANNY_CHECK(suite, rejected && rejected->action_failures == 1);
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>{"twitch_test.enqueue"});
    MANNY_CHECK(suite, fixture.options->snapshot().twitch_test_message.diagnostic ==
                           "Twitch test-message queue is full");
}

void shutdown_tests(TestSuite& suite) {
    constexpr std::string_view marker = "PRIVATE-QUEUED-KEY";
    Fixture fixture;
    MANNY_CHECK(suite, fixture.options
                           ->submit(application::VerifyDonBotCommand{
                               .api_base_url = "https://donbot.example",
                               .api_key = support::SecretValue::from_text(marker),
                           })
                           .has_value());
    fixture.options->shutdown();
    fixture.options->shutdown();
    MANNY_CHECK(suite, fixture.options->is_shutting_down());
    MANNY_CHECK(suite, fixture.options->snapshot().pending_commands == 0);
    MANNY_CHECK(suite, !snapshot_contains(fixture.options->snapshot(), marker));
    MANNY_CHECK(suite, fixture.events.empty());
    MANNY_CHECK(suite, fixture.twitch_test_messenger.cancel_calls == 1);
    MANNY_CHECK(suite, fixture.options->snapshot().twitch_test_message.state ==
                           application::TwitchTestMessageState::ShuttingDown);
    MANNY_CHECK(suite, fixture.donbot_verifier.requests.empty());
    MANNY_CHECK(suite, !fixture.options->tick().has_value());
    MANNY_CHECK(suite, !fixture.options->submit(application::ConnectTwitchCommand{}).has_value());
}

void configuration_tests(TestSuite& suite) {
    Fixture fixture;
    const auto zero_capacity = application::NexusOptionsController::create(
        *fixture.configuration, *fixture.donbot, *fixture.twitch, fixture.twitch_test_messenger,
        {.command_capacity = 0, .max_commands_per_tick = 1});
    MANNY_CHECK(suite, !zero_capacity.has_value());
    MANNY_CHECK(suite, zero_capacity.error().code ==
                           application::NexusOptionsErrorCode::InvalidConfiguration);
    const auto excessive_tick = application::NexusOptionsController::create(
        *fixture.configuration, *fixture.donbot, *fixture.twitch, fixture.twitch_test_messenger,
        {.command_capacity = 1, .max_commands_per_tick = 2});
    MANNY_CHECK(suite, !excessive_tick.has_value());
}

} // namespace

void run_nexus_options_controller_tests(TestSuite& suite) {
    render_boundary_and_queue_tests(suite);
    workflow_owned_settings_tests(suite);
    concurrent_submission_tests(suite);
    donbot_workflow_tests(suite);
    twitch_workflow_tests(suite);
    twitch_test_message_workflow_tests(suite);
    shutdown_tests(suite);
    configuration_tests(suite);
}

} // namespace manny_uploader::test
