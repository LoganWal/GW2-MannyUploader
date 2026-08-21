#include "manny_uploader/application/twitch_authentication_controller.hpp"
#include "manny_uploader/providers/twitch_client.hpp"

#include "support/test_suite.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
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

[[nodiscard]] ports::SecretStoreError secret_error(ports::SecretStoreErrorCode code) {
    return ports::SecretStoreError{
        .code = code,
        .id = ports::SecretId::TwitchOAuthSession,
        .message = "protected Twitch session operation failed",
        .system_error = std::nullopt,
    };
}

class FakeClock final : public ports::IClock {
  public:
    [[nodiscard]] std::chrono::system_clock::time_point system_now() const noexcept override {
        return system_now_;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const noexcept override {
        return steady_now_;
    }

    void advance(std::chrono::seconds amount) noexcept {
        system_now_ += amount;
        steady_now_ += amount;
    }

    std::chrono::system_clock::time_point system_now_{
        std::chrono::system_clock::time_point{1'800'000'000s}};
    std::chrono::steady_clock::time_point steady_now_{std::chrono::steady_clock::time_point{100s}};
};

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
    save(const config::Settings& value) const override {
        events_.push_back("settings.save");
        if (fail_save) {
            return std::unexpected(settings_error());
        }
        saved.push_back(value);
        return {};
    }

    config::Settings initial_;
    std::vector<std::string>& events_;
    mutable std::vector<config::Settings> saved;
    bool fail_save{};
};

class RecordingSecretStore final : public ports::ISecretStore {
  public:
    RecordingSecretStore(std::optional<std::vector<std::byte>> initial,
                         std::vector<std::string>& events)
        : value_{std::move(initial)}, events_{events} {}

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        events_.push_back("secret.load");
        load_ids.push_back(id);
        if (fail_load) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::UnprotectionFailed));
        }
        if (!value_) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::NotFound));
        }
        return support::SecretValue{*value_};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId id, const support::SecretValue& value) override {
        events_.push_back("secret.store");
        store_ids.push_back(id);
        if (fail_store) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::ProtectionFailed));
        }
        value_.emplace(value.bytes().begin(), value.bytes().end());
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId id) override {
        events_.push_back("secret.erase");
        erase_ids.push_back(id);
        if (fail_erase) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::DeleteFailed));
        }
        value_.reset();
        return {};
    }

    mutable std::optional<std::vector<std::byte>> value_;
    std::vector<std::string>& events_;
    bool fail_load{};
    bool fail_store{};
    bool fail_erase{};
    mutable std::vector<ports::SecretId> load_ids;
    std::vector<ports::SecretId> store_ids;
    std::vector<ports::SecretId> erase_ids;
};

[[nodiscard]] std::string operation_name(ports::TwitchAuthenticationOperation operation) {
    switch (operation) {
    case ports::TwitchAuthenticationOperation::Start:
        return "start";
    case ports::TwitchAuthenticationOperation::Poll:
        return "poll";
    case ports::TwitchAuthenticationOperation::Validate:
        return "validate";
    case ports::TwitchAuthenticationOperation::Refresh:
        return "refresh";
    case ports::TwitchAuthenticationOperation::Revoke:
        return "revoke";
    }
    return "unknown";
}

class FakeAuthenticator final : public ports::ITwitchAuthenticator {
  public:
    explicit FakeAuthenticator(std::vector<std::string>& events) : events_{events} {}

    [[nodiscard]] std::expected<void, ports::TwitchAuthenticationDispatchError>
    enqueue(ports::TwitchAuthenticationRequest request) override {
        const auto operation = ports::authentication_operation(request.command);
        events_.push_back("auth.enqueue." + operation_name(operation));
        if (stopping_ || reject_next) {
            reject_next = false;
            return std::unexpected(
                ports::TwitchAuthenticationDispatchError{.message = "queue rejected"});
        }
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
        stopping_ = true;
        requests.clear();
        results.clear();
    }

    void respond_start(std::string device = "PRIVATE-DEVICE-CODE", std::string user = "ABCD-EFGH",
                       std::chrono::seconds expires = 600s, std::chrono::seconds interval = 5s) {
        auto request = take_request(ports::TwitchAuthenticationOperation::Start);
        succeed(std::move(request), ports::TwitchAuthorizationStarted{
                                        .device_code = support::SecretValue::from_text(device),
                                        .user_code = std::move(user),
                                        .verification_uri = "https://www.twitch.tv/activate",
                                        .expires_in = expires,
                                        .polling_interval = interval,
                                    });
    }

    void respond_pending() {
        auto request = take_request(ports::TwitchAuthenticationOperation::Poll);
        auto& poll = std::get<ports::TwitchPollAuthentication>(request.command);
        auto response = ports::TwitchAuthorizationPending{
            .device_code = std::move(poll.device_code),
        };
        succeed(std::move(request), std::move(response));
    }

    void respond_grant(std::string access = "PRIVATE-ACCESS",
                       std::string refresh = "PRIVATE-REFRESH",
                       std::chrono::seconds expires = 14'400s) {
        auto request = take_request(ports::TwitchAuthenticationOperation::Poll);
        succeed(std::move(request), ports::TwitchAuthorizationGranted{
                                        .access_token = support::SecretValue::from_text(access),
                                        .refresh_token = support::SecretValue::from_text(refresh),
                                        .expires_in = expires,
                                        .scopes = {"user:write:chat"},
                                    });
    }

    void respond_validation(std::string user_id = "141981764",
                            std::string login = "broadcaster_name",
                            std::chrono::seconds expires = 14'400s) {
        auto request = take_request(ports::TwitchAuthenticationOperation::Validate);
        auto& validation = std::get<ports::TwitchValidateAuthentication>(request.command);
        auto response = ports::TwitchValidationSucceeded{
            .credentials = std::move(validation.credentials),
            .user_id = std::move(user_id),
            .login = std::move(login),
            .expires_in = expires,
            .scopes = {"user:write:chat"},
        };
        succeed(std::move(request), std::move(response));
    }

    void respond_refresh(std::string access = "ROTATED-ACCESS",
                         std::string refresh = "ROTATED-REFRESH",
                         std::chrono::seconds expires = 14'400s) {
        auto request = take_request(ports::TwitchAuthenticationOperation::Refresh);
        succeed(std::move(request), ports::TwitchRefreshSucceeded{
                                        .access_token = support::SecretValue::from_text(access),
                                        .refresh_token = support::SecretValue::from_text(refresh),
                                        .expires_in = expires,
                                        .scopes = {"user:write:chat"},
                                    });
    }

    void respond_revoke() {
        auto request = take_request(ports::TwitchAuthenticationOperation::Revoke);
        succeed(std::move(request), ports::TwitchRevocationSucceeded{});
    }

    void fail_next(ports::TwitchAuthenticationFailureCode code,
                   std::string detail = "Twitch failed",
                   std::optional<std::chrono::seconds> retry_after = std::nullopt) {
        auto request = std::move(requests.front());
        requests.pop_front();
        auto failure = ports::TwitchAuthenticationFailure{
            .code = code,
            .detail = std::move(detail),
            .retry_after = retry_after,
            .device_code = std::nullopt,
            .credentials = std::nullopt,
        };
        std::visit(
            [&failure]<typename Command>(Command& command) {
                if constexpr (std::is_same_v<Command, ports::TwitchPollAuthentication>) {
                    failure.device_code.emplace(std::move(command.device_code));
                } else if constexpr (std::is_same_v<Command, ports::TwitchValidateAuthentication> ||
                                     std::is_same_v<Command, ports::TwitchRefreshAuthentication>) {
                    failure.credentials.emplace(std::move(command.credentials));
                }
            },
            request.command);
        results.push_back(ports::TwitchAuthenticationResult{
            .request_id = request.request_id,
            .operation = ports::authentication_operation(request.command),
            .outcome = std::unexpected(std::move(failure)),
        });
    }

    void push_stale() {
        results.push_back(ports::TwitchAuthenticationResult{
            .request_id = 9999,
            .operation = ports::TwitchAuthenticationOperation::Start,
            .outcome =
                ports::TwitchAuthenticationSuccess{
                    std::in_place_type<ports::TwitchRevocationSucceeded>},
        });
    }

    template <typename Success>
    void succeed(ports::TwitchAuthenticationRequest request, Success success) {
        results.push_back(ports::TwitchAuthenticationResult{
            .request_id = request.request_id,
            .operation = ports::authentication_operation(request.command),
            .outcome =
                ports::TwitchAuthenticationSuccess{std::in_place_type<Success>, std::move(success)},
        });
    }

    [[nodiscard]] ports::TwitchAuthenticationRequest
    take_request(ports::TwitchAuthenticationOperation expected) {
        auto request = std::move(requests.front());
        requests.pop_front();
        if (ports::authentication_operation(request.command) != expected) {
            unexpected_operation = true;
        }
        return request;
    }

    std::vector<std::string>& events_;
    std::deque<ports::TwitchAuthenticationRequest> requests;
    std::deque<ports::TwitchAuthenticationResult> results;
    bool reject_next{};
    bool stopping_{};
    bool unexpected_operation{};
    std::size_t cancel_calls{};
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

[[nodiscard]] std::optional<std::vector<std::byte>>
encoded_session(std::string access = "SAVED-ACCESS", std::string refresh = "SAVED-REFRESH",
                std::string user_id = "141981764", std::string login = "saved_broadcaster") {
    auto encoded = application::encode_twitch_session(ports::TwitchSession{
        .credentials =
            ports::TwitchCredentialSet{
                .access_token = support::SecretValue::from_text(access),
                .refresh_token = support::SecretValue::from_text(refresh),
                .access_expires_at = std::chrono::system_clock::time_point{1'800'014'400s},
            },
        .user_id = std::move(user_id),
        .login = std::move(login),
        .scopes = {"user:write:chat"},
    });
    if (!encoded) {
        return std::nullopt;
    }
    return std::vector<std::byte>{encoded->bytes().begin(), encoded->bytes().end()};
}

struct Fixture {
    explicit Fixture(std::optional<std::vector<std::byte>> initial_secret = std::nullopt,
                     config::Settings initial_settings = config::make_default_settings("C:/logs"))
        : authenticator{events} {
        auto settings =
            std::make_unique<RecordingSettingsStore>(std::move(initial_settings), events);
        settings_observer = settings.get();
        auto secrets = std::make_unique<RecordingSecretStore>(std::move(initial_secret), events);
        secret_observer = secrets.get();
        auto created =
            application::ConfigurationService::create(std::move(settings), std::move(secrets));
        configuration.emplace(std::move(*created));
        session_owner.emplace(*configuration, client, clock);
        auto controller_created = application::TwitchAuthenticationController::create(
            *configuration, authenticator, *session_owner, clock);
        controller.emplace(std::move(*controller_created));
        events.clear();
    }

    std::vector<std::string> events;
    RecordingSettingsStore* settings_observer{};
    RecordingSecretStore* secret_observer{};
    std::optional<application::ConfigurationService> configuration;
    UnusedTwitchClient client;
    FakeAuthenticator authenticator;
    FakeClock clock;
    std::optional<application::TwitchSessionOwner> session_owner;
    std::optional<application::TwitchAuthenticationController> controller;
};

[[nodiscard]] bool snapshot_contains(const application::TwitchConnectionSnapshot& snapshot,
                                     std::string_view marker) {
    const auto contains = [marker](std::string_view value) {
        return value.find(marker) != std::string_view::npos;
    };
    return contains(snapshot.login.value_or("")) || contains(snapshot.user_code.value_or("")) ||
           contains(snapshot.verification_uri.value_or("")) || contains(snapshot.diagnostic);
}

[[nodiscard]] bool tick_is(application::TwitchAuthenticationController& controller, bool expected) {
    const auto result = controller.tick();
    return result.has_value() && *result == expected;
}

[[nodiscard]] bool begin_saved_is(application::TwitchAuthenticationController& controller,
                                  bool expected) {
    const auto result = controller.begin_saved_connection();
    return result.has_value() && *result == expected;
}

void complete_new_connection(Fixture& fixture, TestSuite& suite,
                             std::chrono::seconds validated_expiry = 14'400s) {
    auto& controller = *fixture.controller;
    MANNY_CHECK(suite, controller.begin_connection().has_value());
    fixture.authenticator.respond_start();
    MANNY_CHECK(suite, controller.tick().has_value());
    fixture.clock.advance(5s);
    MANNY_CHECK(suite, controller.tick().has_value());
    fixture.authenticator.respond_grant();
    MANNY_CHECK(suite, controller.tick().has_value());
    fixture.authenticator.respond_validation("141981764", "broadcaster_name", validated_expiry);
    MANNY_CHECK(suite, controller.tick().has_value());
    MANNY_CHECK(suite,
                controller.snapshot().state == application::TwitchConnectionState::Connected);
}

void new_connection_and_polling_tests(TestSuite& suite) {
    Fixture fixture;
    auto& controller = *fixture.controller;
    MANNY_CHECK(suite,
                controller.snapshot().state == application::TwitchConnectionState::Disconnected);
    MANNY_CHECK(suite, controller.begin_connection().has_value());
    MANNY_CHECK(suite, fixture.authenticator.requests.size() == 1);
    MANNY_CHECK(suite, fixture.secret_observer->store_ids.empty());
    MANNY_CHECK(suite, controller.snapshot().state == application::TwitchConnectionState::Starting);

    fixture.authenticator.respond_start("PRIVATE-DEVICE-CODE");
    MANNY_CHECK(suite, controller.tick().has_value());
    const auto awaiting = controller.snapshot();
    MANNY_CHECK(suite, awaiting.state == application::TwitchConnectionState::AwaitingUser);
    MANNY_CHECK(suite, awaiting.user_code == "ABCD-EFGH");
    MANNY_CHECK(suite, awaiting.verification_uri == "https://www.twitch.tv/activate");
    MANNY_CHECK(suite, awaiting.authorization_expires_at.has_value());
    MANNY_CHECK(suite, !snapshot_contains(awaiting, "PRIVATE-DEVICE-CODE"));
    MANNY_CHECK(suite, tick_is(controller, false));
    MANNY_CHECK(suite, fixture.authenticator.requests.empty());

    fixture.clock.advance(5s);
    MANNY_CHECK(suite, tick_is(controller, true));
    MANNY_CHECK(suite, fixture.authenticator.requests.size() == 1);
    fixture.authenticator.respond_pending();
    MANNY_CHECK(suite, tick_is(controller, true));
    MANNY_CHECK(suite, fixture.authenticator.requests.empty());
    fixture.clock.advance(5s);
    MANNY_CHECK(suite, tick_is(controller, true));
    fixture.authenticator.respond_grant("PRIVATE-ACCESS", "PRIVATE-REFRESH");
    MANNY_CHECK(suite, tick_is(controller, true));
    MANNY_CHECK(suite, fixture.authenticator.requests.size() == 1);
    MANNY_CHECK(suite, fixture.secret_observer->store_ids.empty());
    MANNY_CHECK(suite, !controller.snapshot().user_code.has_value());

    fixture.authenticator.respond_validation();
    fixture.events.clear();
    MANNY_CHECK(suite, tick_is(controller, true));
    MANNY_CHECK(suite, fixture.events == std::vector<std::string>({"secret.store"}));
    const auto connected = controller.snapshot();
    MANNY_CHECK(suite, connected.state == application::TwitchConnectionState::Connected);
    MANNY_CHECK(suite, connected.login == "broadcaster_name");
    MANNY_CHECK(suite, connected.access_expires_at.has_value());
    MANNY_CHECK(suite, !snapshot_contains(connected, "PRIVATE-ACCESS"));
    MANNY_CHECK(suite, !snapshot_contains(connected, "PRIVATE-REFRESH"));
    MANNY_CHECK(suite, fixture.secret_observer->value_.has_value());
    auto stored =
        application::decode_twitch_session(support::SecretValue{*fixture.secret_observer->value_});
    MANNY_CHECK(suite, stored.has_value());
    MANNY_CHECK(suite, stored && stored->login == "broadcaster_name");
    MANNY_CHECK(suite, !fixture.authenticator.unexpected_operation);
}

void saved_session_and_refresh_tests(TestSuite& suite) {
    Fixture missing;
    auto no_saved = missing.controller->begin_saved_connection();
    MANNY_CHECK(suite, no_saved.has_value() && !*no_saved);
    MANNY_CHECK(suite, missing.authenticator.requests.empty());

    Fixture corrupt{std::vector<std::byte>{std::byte{1}, std::byte{2}}};
    const auto invalid = corrupt.controller->begin_saved_connection();
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite,
                invalid.error().code ==
                    application::TwitchAuthenticationControllerErrorCode::SessionDecodeFailed);
    MANNY_CHECK(suite, corrupt.authenticator.requests.empty());

    Fixture restored{encoded_session()};
    MANNY_CHECK(suite, begin_saved_is(*restored.controller, true));
    MANNY_CHECK(suite, restored.controller->snapshot().state ==
                           application::TwitchConnectionState::Validating);
    MANNY_CHECK(suite, !restored.controller->snapshot().login.has_value());
    restored.authenticator.respond_validation("141981764", "renamed_broadcaster");
    restored.events.clear();
    MANNY_CHECK(suite, tick_is(*restored.controller, true));
    MANNY_CHECK(suite, restored.events == std::vector<std::string>({"secret.store"}));
    MANNY_CHECK(suite, restored.controller->snapshot().login == "renamed_broadcaster");

    Fixture recovery{encoded_session()};
    MANNY_CHECK(suite, begin_saved_is(*recovery.controller, true));
    recovery.authenticator.fail_next(ports::TwitchAuthenticationFailureCode::Reconnect);
    recovery.events.clear();
    MANNY_CHECK(suite, tick_is(*recovery.controller, true));
    MANNY_CHECK(suite, recovery.events == std::vector<std::string>({"auth.enqueue.refresh"}));
    recovery.authenticator.respond_refresh("ROTATED-ACCESS", "ROTATED-REFRESH");
    recovery.events.clear();
    MANNY_CHECK(suite, tick_is(*recovery.controller, true));
    MANNY_CHECK(suite, recovery.events ==
                           std::vector<std::string>({"secret.store", "auth.enqueue.validate"}));
    auto rotated =
        application::decode_twitch_session(support::SecretValue{*recovery.secret_observer->value_});
    MANNY_CHECK(suite, rotated.has_value());
    MANNY_CHECK(suite, rotated && rotated->login == "saved_broadcaster");
    recovery.authenticator.respond_validation();
    recovery.events.clear();
    MANNY_CHECK(suite, tick_is(*recovery.controller, true));
    MANNY_CHECK(suite, recovery.events == std::vector<std::string>({"secret.store"}));
    MANNY_CHECK(suite, recovery.controller->snapshot().state ==
                           application::TwitchConnectionState::Connected);

    Fixture second_reconnect{encoded_session()};
    MANNY_CHECK(suite, begin_saved_is(*second_reconnect.controller, true));
    second_reconnect.authenticator.fail_next(ports::TwitchAuthenticationFailureCode::Reconnect);
    MANNY_CHECK(suite, tick_is(*second_reconnect.controller, true));
    second_reconnect.authenticator.respond_refresh();
    MANNY_CHECK(suite, tick_is(*second_reconnect.controller, true));
    second_reconnect.authenticator.fail_next(ports::TwitchAuthenticationFailureCode::Reconnect);
    second_reconnect.events.clear();
    const auto rejected_rotation = second_reconnect.controller->tick();
    MANNY_CHECK(suite, !rejected_rotation.has_value());
    MANNY_CHECK(suite, second_reconnect.events == std::vector<std::string>({"secret.erase"}));
    MANNY_CHECK(suite, !second_reconnect.secret_observer->value_.has_value());
    MANNY_CHECK(suite, second_reconnect.controller->snapshot().state ==
                           application::TwitchConnectionState::Error);

    Fixture rotation_store_failure{encoded_session()};
    MANNY_CHECK(suite, begin_saved_is(*rotation_store_failure.controller, true));
    rotation_store_failure.authenticator.fail_next(
        ports::TwitchAuthenticationFailureCode::Reconnect);
    MANNY_CHECK(suite, tick_is(*rotation_store_failure.controller, true));
    rotation_store_failure.authenticator.respond_refresh();
    rotation_store_failure.secret_observer->fail_store = true;
    rotation_store_failure.events.clear();
    const auto rotation_not_saved = rotation_store_failure.controller->tick();
    MANNY_CHECK(suite, !rotation_not_saved.has_value());
    MANNY_CHECK(suite,
                rotation_not_saved.error().code ==
                    application::TwitchAuthenticationControllerErrorCode::SessionStoreFailed);
    MANNY_CHECK(suite, rotation_store_failure.events ==
                           std::vector<std::string>({"secret.store", "secret.erase"}));
    MANNY_CHECK(suite, rotation_store_failure.authenticator.requests.empty());
    MANNY_CHECK(suite, !rotation_store_failure.secret_observer->value_.has_value());
}

void scheduling_retry_and_identity_tests(TestSuite& suite) {
    Fixture poll_retry;
    MANNY_CHECK(suite, poll_retry.controller->begin_connection().has_value());
    poll_retry.authenticator.respond_start();
    MANNY_CHECK(suite, poll_retry.controller->tick().has_value());
    poll_retry.clock.advance(5s);
    MANNY_CHECK(suite, tick_is(*poll_retry.controller, true));
    poll_retry.authenticator.fail_next(ports::TwitchAuthenticationFailureCode::Retry,
                                       "Polling will retry", 17s);
    MANNY_CHECK(suite, tick_is(*poll_retry.controller, true));
    MANNY_CHECK(suite, poll_retry.authenticator.requests.empty());
    poll_retry.clock.advance(16s);
    MANNY_CHECK(suite, tick_is(*poll_retry.controller, false));
    poll_retry.clock.advance(1s);
    MANNY_CHECK(suite, tick_is(*poll_retry.controller, true));
    MANNY_CHECK(
        suite, ports::authentication_operation(poll_retry.authenticator.requests.front().command) ==
                   ports::TwitchAuthenticationOperation::Poll);

    Fixture hourly;
    complete_new_connection(hourly, suite, 14'400s);
    hourly.authenticator.requests.clear();
    hourly.clock.advance(1h);
    MANNY_CHECK(suite, tick_is(*hourly.controller, true));
    MANNY_CHECK(suite,
                ports::authentication_operation(hourly.authenticator.requests.front().command) ==
                    ports::TwitchAuthenticationOperation::Validate);

    Fixture pre_expiry;
    complete_new_connection(pre_expiry, suite, 3600s);
    pre_expiry.authenticator.requests.clear();
    pre_expiry.clock.advance(55min);
    MANNY_CHECK(suite, tick_is(*pre_expiry.controller, true));
    MANNY_CHECK(
        suite, ports::authentication_operation(pre_expiry.authenticator.requests.front().command) ==
                   ports::TwitchAuthenticationOperation::Refresh);

    Fixture retry{encoded_session()};
    MANNY_CHECK(suite, begin_saved_is(*retry.controller, true));
    retry.authenticator.fail_next(ports::TwitchAuthenticationFailureCode::Retry,
                                  "Validation will retry", 17s);
    MANNY_CHECK(suite, tick_is(*retry.controller, true));
    MANNY_CHECK(suite, retry.authenticator.requests.empty());
    retry.clock.advance(16s);
    MANNY_CHECK(suite, tick_is(*retry.controller, false));
    retry.clock.advance(1s);
    MANNY_CHECK(suite, tick_is(*retry.controller, true));
    MANNY_CHECK(suite,
                ports::authentication_operation(retry.authenticator.requests.front().command) ==
                    ports::TwitchAuthenticationOperation::Validate);

    Fixture mismatch{encoded_session()};
    MANNY_CHECK(suite, begin_saved_is(*mismatch.controller, true));
    mismatch.authenticator.respond_validation("999999", "different_broadcaster");
    mismatch.events.clear();
    const auto identity_error = mismatch.controller->tick();
    MANNY_CHECK(suite, !identity_error.has_value());
    MANNY_CHECK(suite, identity_error.error().code ==
                           application::TwitchAuthenticationControllerErrorCode::IdentityMismatch);
    MANNY_CHECK(suite, mismatch.events == std::vector<std::string>({"secret.erase"}));
    MANNY_CHECK(suite, !mismatch.secret_observer->value_.has_value());
}

void persistence_disconnect_and_shutdown_tests(TestSuite& suite) {
    Fixture persistence;
    auto& controller = *persistence.controller;
    MANNY_CHECK(suite, controller.begin_connection().has_value());
    persistence.authenticator.respond_start();
    MANNY_CHECK(suite, controller.tick().has_value());
    persistence.clock.advance(5s);
    MANNY_CHECK(suite, controller.tick().has_value());
    persistence.authenticator.respond_grant();
    MANNY_CHECK(suite, controller.tick().has_value());
    persistence.secret_observer->fail_store = true;
    persistence.authenticator.respond_validation();
    const auto store_failed = controller.tick();
    MANNY_CHECK(suite, !store_failed.has_value());
    MANNY_CHECK(suite,
                store_failed.error().code ==
                    application::TwitchAuthenticationControllerErrorCode::SessionStoreFailed);
    MANNY_CHECK(suite, controller.snapshot().state == application::TwitchConnectionState::Error);
    MANNY_CHECK(suite, !controller.snapshot().login.has_value());

    auto enabled = config::make_default_settings("C:/logs");
    enabled.twitch.enabled = true;
    Fixture disconnecting{encoded_session(), enabled};
    MANNY_CHECK(suite, begin_saved_is(*disconnecting.controller, true));
    disconnecting.authenticator.respond_validation();
    MANNY_CHECK(suite, disconnecting.controller->tick().has_value());
    disconnecting.events.clear();
    MANNY_CHECK(suite, disconnecting.controller->disconnect().has_value());
    MANNY_CHECK(suite, disconnecting.events ==
                           std::vector<std::string>({"settings.save", "auth.enqueue.revoke"}));
    MANNY_CHECK(suite, disconnecting.controller->snapshot().state ==
                           application::TwitchConnectionState::Disconnecting);
    MANNY_CHECK(suite, disconnecting.secret_observer->value_.has_value());
    disconnecting.authenticator.fail_next(ports::TwitchAuthenticationFailureCode::Retry,
                                          "network unavailable");
    disconnecting.events.clear();
    const auto revoked = disconnecting.controller->tick();
    MANNY_CHECK(suite, !revoked.has_value());
    MANNY_CHECK(suite, disconnecting.events == std::vector<std::string>({"secret.erase"}));
    MANNY_CHECK(suite, !disconnecting.secret_observer->value_.has_value());
    MANNY_CHECK(suite, disconnecting.controller->snapshot().state ==
                           application::TwitchConnectionState::Disconnected);
    MANNY_CHECK(suite, !disconnecting.configuration->snapshot().settings.twitch.enabled);

    Fixture save_failure{encoded_session(), enabled};
    MANNY_CHECK(suite, begin_saved_is(*save_failure.controller, true));
    save_failure.authenticator.respond_validation();
    MANNY_CHECK(suite, save_failure.controller->tick().has_value());
    save_failure.settings_observer->fail_save = true;
    save_failure.events.clear();
    const auto not_disabled = save_failure.controller->disconnect();
    MANNY_CHECK(suite, !not_disabled.has_value());
    MANNY_CHECK(suite, save_failure.events == std::vector<std::string>({"settings.save"}));
    MANNY_CHECK(suite, save_failure.authenticator.requests.empty());
    MANNY_CHECK(suite, save_failure.secret_observer->value_.has_value());

    Fixture expiry;
    MANNY_CHECK(suite, expiry.controller->begin_connection().has_value());
    expiry.authenticator.respond_start("DEVICE", "CODE", 10s, 5s);
    MANNY_CHECK(suite, expiry.controller->tick().has_value());
    expiry.clock.advance(10s);
    const auto expired = expiry.controller->tick();
    MANNY_CHECK(suite, !expired.has_value());
    MANNY_CHECK(suite,
                expired.error().code ==
                    application::TwitchAuthenticationControllerErrorCode::AuthorizationExpired);
    MANNY_CHECK(suite, expiry.authenticator.requests.empty());

    Fixture stale;
    MANNY_CHECK(suite, stale.controller->begin_connection().has_value());
    stale.authenticator.push_stale();
    const auto stale_result = stale.controller->tick();
    MANNY_CHECK(suite, !stale_result.has_value());
    MANNY_CHECK(suite, stale_result.error().code ==
                           application::TwitchAuthenticationControllerErrorCode::StaleResult);
    MANNY_CHECK(suite,
                stale.controller->snapshot().state == application::TwitchConnectionState::Starting);

    stale.controller->shutdown();
    MANNY_CHECK(suite, stale.controller->is_shutting_down());
    MANNY_CHECK(suite, stale.authenticator.cancel_calls == 1);
    MANNY_CHECK(suite, stale.controller->snapshot().state ==
                           application::TwitchConnectionState::ShuttingDown);
    MANNY_CHECK(suite, !stale.controller->snapshot().login.has_value());
    MANNY_CHECK(suite, !stale.controller->begin_connection().has_value());
    stale.controller->shutdown();
    MANNY_CHECK(suite, stale.authenticator.cancel_calls == 1);
}

} // namespace

void run_twitch_authentication_controller_tests(TestSuite& suite) {
    new_connection_and_polling_tests(suite);
    saved_session_and_refresh_tests(suite);
    scheduling_retry_and_identity_tests(suite);
    persistence_disconnect_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
