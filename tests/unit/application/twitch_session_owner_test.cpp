#include "manny_uploader/application/twitch_session_owner.hpp"

#include "manny_uploader/providers/twitch_client.hpp"
#include "support/fakes.hpp"
#include "support/test_suite.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] providers::TwitchError twitch_error(providers::TwitchDisposition disposition,
                                                  std::string detail = "Twitch failed") {
    return providers::TwitchError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = std::nullopt,
        .http_error = std::nullopt,
        .http_status = std::nullopt,
    };
}

[[nodiscard]] ports::SecretStoreError secret_error(ports::SecretStoreErrorCode code) {
    return ports::SecretStoreError{
        .code = code,
        .id = ports::SecretId::TwitchOAuthSession,
        .message = "secret operation failed",
        .system_error = std::nullopt,
    };
}

class MemorySettingsStore final : public ports::ISettingsStore {
  public:
    [[nodiscard]] std::expected<ports::SettingsLoadResult, ports::SettingsStoreError>
    load() const override {
        return ports::SettingsLoadResult{
            .settings = config::make_default_settings("C:/logs"),
            .source = ports::SettingsLoadSource::Primary,
            .recovery_diagnostic = std::nullopt,
        };
    }

    [[nodiscard]] std::expected<void, ports::SettingsStoreError>
    save(const config::Settings&) const override {
        return {};
    }
};

class MemorySecretStore final : public ports::ISecretStore {
  public:
    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId) const override {
        const std::scoped_lock lock{mutex_};
        if (!value) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::NotFound));
        }
        return support::SecretValue{*value};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId, const support::SecretValue& secret) override {
        const std::scoped_lock lock{mutex_};
        ++store_calls;
        if (fail_store) {
            return std::unexpected(secret_error(ports::SecretStoreErrorCode::ProtectionFailed));
        }
        value.emplace(secret.bytes().begin(), secret.bytes().end());
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId) override {
        const std::scoped_lock lock{mutex_};
        ++erase_calls;
        value.reset();
        return {};
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> copy_value() const {
        const std::scoped_lock lock{mutex_};
        return value;
    }

    mutable std::mutex mutex_;
    std::optional<std::vector<std::byte>> value;
    std::size_t store_calls{};
    std::size_t erase_calls{};
    bool fail_store{};
};

class ScriptedTwitchClient final : public providers::ITwitchClient {
  public:
    using Validation = std::expected<providers::TwitchValidatedIdentity, providers::TwitchError>;
    using Refresh = std::expected<providers::TwitchTokenGrant, providers::TwitchError>;

    [[nodiscard]] std::expected<providers::TwitchDeviceAuthorization, providers::TwitchError>
    start_device_authorization(const std::stop_token&) const override {
        return std::unexpected(twitch_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] std::expected<providers::TwitchDevicePollResult, providers::TwitchError>
    poll_device_authorization(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(twitch_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] Validation validate_access_token(const support::SecretValue&,
                                                   const std::stop_token&) const override {
        std::unique_lock lock{mutex_};
        ++validation_calls;
        if (block_validation) {
            validation_started = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return release_validation; });
        }
        if (validations.empty()) {
            return std::unexpected(
                twitch_error(providers::TwitchDisposition::Failed, "missing validation script"));
        }
        auto result = std::move(validations.front());
        validations.pop_front();
        return result;
    }

    [[nodiscard]] Refresh refresh_access_token(const support::SecretValue&,
                                               const std::stop_token&) const override {
        const std::scoped_lock lock{mutex_};
        ++refresh_calls;
        if (refreshes.empty()) {
            return std::unexpected(
                twitch_error(providers::TwitchDisposition::Failed, "missing refresh script"));
        }
        auto result = std::move(refreshes.front());
        refreshes.pop_front();
        return result;
    }

    [[nodiscard]] std::expected<void, providers::TwitchError>
    revoke_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return {};
    }

    [[nodiscard]] std::expected<providers::TwitchChatResult, providers::TwitchError>
    send_chat_message(std::string_view, std::string_view, const support::SecretValue&,
                      const std::stop_token&) const override {
        return std::unexpected(twitch_error(providers::TwitchDisposition::Failed));
    }

    void wait_for_validation() const {
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this] { return validation_started; });
    }

    void release() {
        const std::scoped_lock lock{mutex_};
        release_validation = true;
        condition_.notify_all();
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::deque<Validation> validations;
    mutable std::deque<Refresh> refreshes;
    mutable std::size_t validation_calls{};
    mutable std::size_t refresh_calls{};
    mutable bool block_validation{};
    mutable bool validation_started{};
    mutable bool release_validation{};
};

[[nodiscard]] ports::TwitchSession session(std::string access = "ACCESS-ONE",
                                           std::string refresh = "REFRESH-ONE") {
    return ports::TwitchSession{
        .credentials =
            ports::TwitchCredentialSet{
                .access_token = support::SecretValue::from_text(access),
                .refresh_token = support::SecretValue::from_text(refresh),
                .access_expires_at = std::chrono::system_clock::time_point{14'400s},
            },
        .user_id = "141981764",
        .login = "broadcaster_name",
        .scopes = {"user:write:chat"},
    };
}

[[nodiscard]] providers::TwitchValidatedIdentity validated(std::string user_id = "141981764",
                                                           std::string login = "broadcaster_name") {
    return providers::TwitchValidatedIdentity{
        .user_id = std::move(user_id),
        .login = std::move(login),
        .expires_in = 14'400s,
        .scopes = {"user:write:chat"},
    };
}

[[nodiscard]] providers::TwitchTokenGrant refreshed(std::string access = "ACCESS-ROTATED",
                                                    std::string refresh = "REFRESH-ROTATED") {
    return providers::TwitchTokenGrant{
        .access_token = support::SecretValue::from_text(access),
        .refresh_token = support::SecretValue::from_text(refresh),
        .expires_in = 14'400s,
        .scopes = {"user:write:chat"},
    };
}

struct Fixture {
    Fixture() {
        auto secrets = std::make_unique<MemorySecretStore>();
        secret_observer = secrets.get();
        auto created = application::ConfigurationService::create(
            std::make_unique<MemorySettingsStore>(), std::move(secrets));
        configuration.emplace(std::move(*created));
        owner.emplace(*configuration, client, clock);
    }

    [[nodiscard]] ports::TwitchDeliverySession activate(ports::TwitchSession value = session()) {
        auto transaction = owner->begin_new(std::move(value));
        auto committed = owner->commit(transaction->id, std::move(transaction->session));
        return *owner->acquire({});
    }

    FakeClock clock;
    ScriptedTwitchClient client;
    MemorySecretStore* secret_observer{};
    std::optional<application::ConfigurationService> configuration;
    std::optional<application::TwitchSessionOwner> owner;
};

void transaction_and_stale_lease_tests(TestSuite& suite) {
    Fixture fixture;
    auto old_lease = fixture.activate();
    MANNY_CHECK(suite, old_lease.revision == 1);

    auto transaction = fixture.owner->checkout_active();
    MANNY_CHECK(suite, transaction.has_value());
    const auto unavailable = fixture.owner->acquire({});
    MANNY_CHECK(suite, !unavailable.has_value());
    MANNY_CHECK(suite, unavailable.error().code == ports::TwitchDeliverySessionErrorCode::Retry);

    transaction->session.credentials.access_token = support::SecretValue::from_text("ACCESS-TWO");
    transaction->session.credentials.refresh_token = support::SecretValue::from_text("REFRESH-TWO");
    auto committed = fixture.owner->commit(transaction->id, std::move(transaction->session));
    MANNY_CHECK(suite, committed.has_value());
    MANNY_CHECK(suite, committed && committed->revision == 2);

    const auto recovered_old = fixture.owner->recover(std::move(old_lease), {});
    MANNY_CHECK(suite, recovered_old.has_value());
    MANNY_CHECK(suite, recovered_old && recovered_old->revision == 2);
    MANNY_CHECK(suite, fixture.client.validation_calls == 0);
    MANNY_CHECK(suite, fixture.client.refresh_calls == 0);
}

void rotation_and_identity_tests(TestSuite& suite) {
    Fixture fixture;
    auto lease = fixture.activate();
    const auto stores_before = fixture.secret_observer->store_calls;
    fixture.client.validations.emplace_back(
        std::unexpected(twitch_error(providers::TwitchDisposition::Reconnect, "expired")));
    fixture.client.refreshes.emplace_back(refreshed());
    fixture.client.validations.emplace_back(validated());

    auto recovered = fixture.owner->recover(std::move(lease), {});
    MANNY_CHECK(suite, recovered.has_value());
    MANNY_CHECK(suite, recovered && recovered->revision == 2);
    MANNY_CHECK(suite, fixture.client.validation_calls == 2);
    MANNY_CHECK(suite, fixture.client.refresh_calls == 1);
    MANNY_CHECK(suite, fixture.secret_observer->store_calls == stores_before + 2);
    const auto stored_bytes = fixture.secret_observer->copy_value();
    auto stored = application::decode_twitch_session(support::SecretValue{*stored_bytes});
    MANNY_CHECK(suite, stored.has_value());
    MANNY_CHECK(suite, stored && stored->credentials.access_token ==
                                     support::SecretValue::from_text("ACCESS-ROTATED"));

    auto mismatch_lease = fixture.owner->acquire({});
    fixture.client.validations.emplace_back(validated("999999", "other_broadcaster"));
    auto mismatch = fixture.owner->recover(std::move(*mismatch_lease), {});
    MANNY_CHECK(suite, !mismatch.has_value());
    MANNY_CHECK(suite,
                mismatch.error().code == ports::TwitchDeliverySessionErrorCode::ReconnectRequired);
    MANNY_CHECK(suite, fixture.secret_observer->erase_calls == 1);
    const auto disconnected = fixture.owner->acquire({});
    MANNY_CHECK(suite, !disconnected.has_value());
    MANNY_CHECK(suite,
                disconnected.error().code == ports::TwitchDeliverySessionErrorCode::NotConnected);
}

void recovery_serialization_and_shutdown_tests(TestSuite& suite) {
    Fixture fixture;
    auto lease = fixture.activate();
    fixture.client.validations.emplace_back(validated());
    fixture.client.block_validation = true;
    std::optional<std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>>
        recovery;
    std::jthread worker{[&] { recovery.emplace(fixture.owner->recover(std::move(lease), {})); }};
    fixture.client.wait_for_validation();

    const auto controller_checkout = fixture.owner->checkout_active();
    MANNY_CHECK(suite, !controller_checkout.has_value());
    MANNY_CHECK(suite,
                controller_checkout.error().code == application::TwitchSessionOwnerErrorCode::Busy);
    const auto delivery_checkout = fixture.owner->acquire({});
    MANNY_CHECK(suite, !delivery_checkout.has_value());
    MANNY_CHECK(suite,
                delivery_checkout.error().code == ports::TwitchDeliverySessionErrorCode::Retry);
    fixture.client.release();
    worker.join();
    MANNY_CHECK(suite, recovery.has_value() && recovery->has_value());

    Fixture shutdown;
    auto shutdown_lease = shutdown.activate();
    shutdown.client.validations.emplace_back(validated());
    shutdown.client.block_validation = true;
    std::optional<std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>>
        stopped;
    std::jthread stopping_worker{
        [&] { stopped.emplace(shutdown.owner->recover(std::move(shutdown_lease), {})); }};
    shutdown.client.wait_for_validation();
    shutdown.owner->shutdown();
    shutdown.client.release();
    stopping_worker.join();
    MANNY_CHECK(suite, stopped.has_value() && !stopped->has_value());
    MANNY_CHECK(suite, stopped && stopped->error().code ==
                                      ports::TwitchDeliverySessionErrorCode::Cancelled);
    MANNY_CHECK(suite, shutdown.owner->is_shutting_down());
}

} // namespace

void run_twitch_session_owner_tests(TestSuite& suite) {
    transaction_and_stale_lease_tests(suite);
    rotation_and_identity_tests(suite);
    recovery_serialization_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
