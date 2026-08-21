#include "manny_uploader/providers/twitch_authentication_worker.hpp"

#include "support/test_suite.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] providers::TwitchError client_error(providers::TwitchDisposition disposition,
                                                  std::string detail = "Twitch failed") {
    return providers::TwitchError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = disposition == providers::TwitchDisposition::Retry
                           ? std::optional<std::chrono::seconds>{17s}
                           : std::nullopt,
        .http_error = std::nullopt,
        .http_status = std::nullopt,
    };
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] ports::TwitchCredentialSet credentials(std::string access = "ACCESS",
                                                     std::string refresh = "REFRESH") {
    return ports::TwitchCredentialSet{
        .access_token = support::SecretValue::from_text(access),
        .refresh_token = support::SecretValue::from_text(refresh),
        .access_expires_at = std::chrono::system_clock::time_point{1'800'000'000s},
    };
}

class FakeTwitchClient final : public providers::ITwitchClient {
  public:
    using StartResult = std::expected<providers::TwitchDeviceAuthorization, providers::TwitchError>;
    using PollResult = std::expected<providers::TwitchDevicePollResult, providers::TwitchError>;
    using ValidateResult =
        std::expected<providers::TwitchValidatedIdentity, providers::TwitchError>;
    using RefreshResult = std::expected<providers::TwitchTokenGrant, providers::TwitchError>;
    using RevokeResult = std::expected<void, providers::TwitchError>;

    void push_start(StartResult result) {
        const std::scoped_lock lock{mutex_};
        starts_.push_back(std::move(result));
    }

    void push_poll(PollResult result) {
        const std::scoped_lock lock{mutex_};
        polls_.push_back(std::move(result));
    }

    void push_validate(ValidateResult result) {
        const std::scoped_lock lock{mutex_};
        validations_.push_back(std::move(result));
    }

    void push_refresh(RefreshResult result) {
        const std::scoped_lock lock{mutex_};
        refreshes_.push_back(std::move(result));
    }

    void push_revoke(RevokeResult result) {
        const std::scoped_lock lock{mutex_};
        revocations_.push_back(std::move(result));
    }

    void block() {
        const std::scoped_lock lock{mutex_};
        blocked_ = true;
        released_ = false;
    }

    void release() {
        {
            const std::scoped_lock lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

    void throw_next() {
        const std::scoped_lock lock{mutex_};
        throw_next_ = true;
    }

    [[nodiscard]] bool wait_for_calls(std::size_t count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, timeout, [this, count] { return calls_ >= count; });
    }

    [[nodiscard]] bool saw_stop() const {
        const std::scoped_lock lock{mutex_};
        return saw_stop_;
    }

    [[nodiscard]] std::vector<std::string> operations() const {
        const std::scoped_lock lock{mutex_};
        return operations_;
    }

    [[nodiscard]] std::expected<providers::TwitchDeviceAuthorization, providers::TwitchError>
    start_device_authorization(const std::stop_token& stop_token) const override {
        if (!gate("start", stop_token)) {
            return std::unexpected(
                client_error(providers::TwitchDisposition::Cancelled, "cancelled"));
        }
        const std::scoped_lock lock{mutex_};
        if (!starts_.empty()) {
            auto result = std::move(starts_.front());
            starts_.pop_front();
            return result;
        }
        return providers::TwitchDeviceAuthorization{
            .device_code = support::SecretValue::from_text("DEVICE"),
            .user_code = "ABCD-EFGH",
            .verification_uri = "https://www.twitch.tv/activate",
            .expires_in = 600s,
            .polling_interval = 5s,
        };
    }

    [[nodiscard]] std::expected<providers::TwitchDevicePollResult, providers::TwitchError>
    poll_device_authorization(const support::SecretValue& device_code,
                              const std::stop_token& stop_token) const override {
        capture_secret(device_code);
        if (!gate("poll", stop_token)) {
            return std::unexpected(
                client_error(providers::TwitchDisposition::Cancelled, "cancelled"));
        }
        const std::scoped_lock lock{mutex_};
        if (!polls_.empty()) {
            auto result = std::move(polls_.front());
            polls_.pop_front();
            return result;
        }
        return providers::TwitchDevicePollResult{
            std::in_place_type<providers::TwitchAuthorizationPending>};
    }

    [[nodiscard]] std::expected<providers::TwitchValidatedIdentity, providers::TwitchError>
    validate_access_token(const support::SecretValue& access_token,
                          const std::stop_token& stop_token) const override {
        capture_secret(access_token);
        if (!gate("validate", stop_token)) {
            return std::unexpected(
                client_error(providers::TwitchDisposition::Cancelled, "cancelled"));
        }
        const std::scoped_lock lock{mutex_};
        if (!validations_.empty()) {
            auto result = std::move(validations_.front());
            validations_.pop_front();
            return result;
        }
        return providers::TwitchValidatedIdentity{
            .user_id = "141981764",
            .login = "broadcaster_name",
            .expires_in = 3600s,
            .scopes = {"user:write:chat"},
        };
    }

    [[nodiscard]] std::expected<providers::TwitchTokenGrant, providers::TwitchError>
    refresh_access_token(const support::SecretValue& refresh_token,
                         const std::stop_token& stop_token) const override {
        capture_secret(refresh_token);
        if (!gate("refresh", stop_token)) {
            return std::unexpected(
                client_error(providers::TwitchDisposition::Cancelled, "cancelled"));
        }
        const std::scoped_lock lock{mutex_};
        if (!refreshes_.empty()) {
            auto result = std::move(refreshes_.front());
            refreshes_.pop_front();
            return result;
        }
        return providers::TwitchTokenGrant{
            .access_token = support::SecretValue::from_text("NEW-ACCESS"),
            .refresh_token = support::SecretValue::from_text("NEW-REFRESH"),
            .expires_in = 14'400s,
            .scopes = {"user:write:chat"},
        };
    }

    [[nodiscard]] std::expected<void, providers::TwitchError>
    revoke_access_token(const support::SecretValue& access_token,
                        const std::stop_token& stop_token) const override {
        capture_secret(access_token);
        if (!gate("revoke", stop_token)) {
            return std::unexpected(
                client_error(providers::TwitchDisposition::Cancelled, "cancelled"));
        }
        const std::scoped_lock lock{mutex_};
        if (!revocations_.empty()) {
            auto result = std::move(revocations_.front());
            revocations_.pop_front();
            return result;
        }
        return {};
    }

    [[nodiscard]] std::expected<providers::TwitchChatResult, providers::TwitchError>
    send_chat_message(std::string_view, std::string_view, const support::SecretValue&,
                      const std::stop_token&) const override {
        return std::unexpected(client_error(providers::TwitchDisposition::Failed, "unused"));
    }

  private:
    [[nodiscard]] bool gate(std::string operation, const std::stop_token& stop_token) const {
        std::stop_callback wake_on_stop{stop_token, [this] { condition_.notify_all(); }};
        std::unique_lock lock{mutex_};
        ++calls_;
        operations_.push_back(std::move(operation));
        condition_.notify_all();
        condition_.wait(lock, [this, &stop_token] {
            return !blocked_ || released_ || stop_token.stop_requested();
        });
        if (stop_token.stop_requested()) {
            saw_stop_ = true;
            return false;
        }
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error{"private Twitch exception marker"};
        }
        return true;
    }

    void capture_secret(const support::SecretValue& value) const {
        const std::scoped_lock lock{mutex_};
        captured_secrets_.push_back(secret_text(value));
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::deque<StartResult> starts_;
    mutable std::deque<PollResult> polls_;
    mutable std::deque<ValidateResult> validations_;
    mutable std::deque<RefreshResult> refreshes_;
    mutable std::deque<RevokeResult> revocations_;
    mutable std::vector<std::string> operations_;
    mutable std::vector<std::string> captured_secrets_;
    mutable std::size_t calls_{};
    mutable bool blocked_{};
    mutable bool released_{};
    mutable bool throw_next_{};
    mutable bool saw_stop_{};
};

[[nodiscard]] ports::TwitchAuthenticationRequest
request(std::uint64_t id, ports::TwitchAuthenticationCommand command) {
    return ports::TwitchAuthenticationRequest{.request_id = id, .command = std::move(command)};
}

void operation_and_success_tests(TestSuite& suite) {
    FakeTwitchClient client;
    const auto invalid = providers::TwitchAuthenticationWorker::create(client, 0);
    MANNY_CHECK(suite, !invalid.has_value());
    auto created = providers::TwitchAuthenticationWorker::create(client, 3);
    MANNY_CHECK(suite, created.has_value());
    if (!created) {
        return;
    }
    auto worker = std::move(*created);
    MANNY_CHECK(suite,
                !worker->enqueue(request(0, ports::TwitchStartAuthentication{})).has_value());

    MANNY_CHECK(suite, worker->enqueue(request(1, ports::TwitchStartAuthentication{})).has_value());
    auto started = worker->wait_for_result(2s);
    MANNY_CHECK(suite,
                started && started->operation == ports::TwitchAuthenticationOperation::Start);
    auto* start =
        started ? std::get_if<ports::TwitchAuthorizationStarted>(&*started->outcome) : nullptr;
    MANNY_CHECK(suite, start != nullptr);
    MANNY_CHECK(suite, start && secret_text(start->device_code) == "DEVICE");

    client.push_poll(providers::TwitchDevicePollResult{
        std::in_place_type<providers::TwitchAuthorizationPending>});
    MANNY_CHECK(suite, worker
                           ->enqueue(request(
                               2,
                               ports::TwitchPollAuthentication{
                                   .device_code = support::SecretValue::from_text("POLL-DEVICE")}))
                           .has_value());
    auto pending = worker->wait_for_result(2s);
    auto* pending_value =
        pending ? std::get_if<ports::TwitchAuthorizationPending>(&*pending->outcome) : nullptr;
    MANNY_CHECK(suite, pending_value != nullptr);
    MANNY_CHECK(suite, pending_value && secret_text(pending_value->device_code) == "POLL-DEVICE");

    client.push_poll(providers::TwitchDevicePollResult{
        std::in_place_type<providers::TwitchTokenGrant>,
        providers::TwitchTokenGrant{
            .access_token = support::SecretValue::from_text("GRANTED-ACCESS"),
            .refresh_token = support::SecretValue::from_text("GRANTED-REFRESH"),
            .expires_in = 3600s,
            .scopes = {"user:write:chat"},
        }});
    MANNY_CHECK(suite,
                worker
                    ->enqueue(request(3,
                                      ports::TwitchPollAuthentication{
                                          .device_code = support::SecretValue::from_text("D")}))
                    .has_value());
    auto granted = worker->wait_for_result(2s);
    auto* grant =
        granted ? std::get_if<ports::TwitchAuthorizationGranted>(&*granted->outcome) : nullptr;
    MANNY_CHECK(suite, grant != nullptr);
    MANNY_CHECK(suite, grant && secret_text(grant->access_token) == "GRANTED-ACCESS");
    MANNY_CHECK(suite, grant && secret_text(grant->refresh_token) == "GRANTED-REFRESH");

    MANNY_CHECK(suite, worker
                           ->enqueue(request(4,
                                             ports::TwitchValidateAuthentication{
                                                 .credentials = credentials("VALIDATE", "KEEP")}))
                           .has_value());
    auto validated = worker->wait_for_result(2s);
    auto* identity =
        validated ? std::get_if<ports::TwitchValidationSucceeded>(&*validated->outcome) : nullptr;
    MANNY_CHECK(suite, identity != nullptr);
    MANNY_CHECK(suite, identity && identity->login == "broadcaster_name");
    MANNY_CHECK(suite, identity && secret_text(identity->credentials.refresh_token) == "KEEP");

    MANNY_CHECK(suite, worker
                           ->enqueue(request(5,
                                             ports::TwitchRefreshAuthentication{
                                                 .credentials = credentials("OLD", "ROTATE")}))
                           .has_value());
    auto refreshed = worker->wait_for_result(2s);
    auto* refresh =
        refreshed ? std::get_if<ports::TwitchRefreshSucceeded>(&*refreshed->outcome) : nullptr;
    MANNY_CHECK(suite, refresh != nullptr);
    MANNY_CHECK(suite, refresh && secret_text(refresh->access_token) == "NEW-ACCESS");
    MANNY_CHECK(suite, refresh && secret_text(refresh->refresh_token) == "NEW-REFRESH");

    MANNY_CHECK(suite, worker
                           ->enqueue(request(
                               6,
                               ports::TwitchRevokeAuthentication{
                                   .access_token = support::SecretValue::from_text("REVOKE")}))
                           .has_value());
    auto revoked = worker->wait_for_result(2s);
    MANNY_CHECK(suite, revoked && std::holds_alternative<ports::TwitchRevocationSucceeded>(
                                      *revoked->outcome));
    MANNY_CHECK(suite,
                client.operations() == std::vector<std::string>({"start", "poll", "poll",
                                                                 "validate", "refresh", "revoke"}));
}

void failure_retention_and_exception_tests(TestSuite& suite) {
    FakeTwitchClient client;
    auto created = providers::TwitchAuthenticationWorker::create(client, 2);
    MANNY_CHECK(suite, created.has_value());
    if (!created) {
        return;
    }
    auto worker = std::move(*created);

    client.push_poll(
        std::unexpected(client_error(providers::TwitchDisposition::Retry, "retry poll")));
    MANNY_CHECK(
        suite,
        worker
            ->enqueue(request(11,
                              ports::TwitchPollAuthentication{
                                  .device_code = support::SecretValue::from_text("RETURN-DEVICE")}))
            .has_value());
    auto poll = worker->wait_for_result(2s);
    MANNY_CHECK(suite, poll && !poll->outcome.has_value());
    MANNY_CHECK(suite, poll && poll->outcome.error().code ==
                                   ports::TwitchAuthenticationFailureCode::Retry);
    MANNY_CHECK(suite, poll && poll->outcome.error().retry_after == 17s);
    MANNY_CHECK(suite, poll && poll->outcome.error().device_code &&
                           secret_text(*poll->outcome.error().device_code) == "RETURN-DEVICE");

    client.push_validate(
        std::unexpected(client_error(providers::TwitchDisposition::Reconnect, "reconnect")));
    MANNY_CHECK(suite, worker
                           ->enqueue(request(
                               12,
                               ports::TwitchValidateAuthentication{
                                   .credentials = credentials("RETURN-ACCESS", "RETURN-REFRESH")}))
                           .has_value());
    auto validation = worker->wait_for_result(2s);
    MANNY_CHECK(suite, validation && !validation->outcome.has_value());
    MANNY_CHECK(suite, validation && validation->outcome.error().credentials.has_value());
    MANNY_CHECK(suite,
                validation && secret_text(validation->outcome.error().credentials->access_token) ==
                                  "RETURN-ACCESS");

    client.throw_next();
    MANNY_CHECK(suite, worker
                           ->enqueue(request(
                               13, ports::TwitchRefreshAuthentication{.credentials = credentials(
                                                                          "EXCEPT-A", "EXCEPT-R")}))
                           .has_value());
    auto exception = worker->wait_for_result(2s);
    MANNY_CHECK(suite, exception && !exception->outcome.has_value());
    MANNY_CHECK(suite, exception && exception->outcome.error().credentials.has_value());
    MANNY_CHECK(suite, exception &&
                           exception->outcome.error().detail.find("private") == std::string::npos);
    MANNY_CHECK(suite,
                exception && secret_text(exception->outcome.error().credentials->refresh_token) ==
                                 "EXCEPT-R");
}

void backpressure_and_cancellation_tests(TestSuite& suite) {
    FakeTwitchClient client;
    client.block();
    auto created = providers::TwitchAuthenticationWorker::create(client, 1);
    MANNY_CHECK(suite, created.has_value());
    if (!created) {
        return;
    }
    auto worker = std::move(*created);
    MANNY_CHECK(suite,
                worker->enqueue(request(21, ports::TwitchStartAuthentication{})).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));
    MANNY_CHECK(suite,
                worker->enqueue(request(22, ports::TwitchStartAuthentication{})).has_value());
    MANNY_CHECK(suite,
                !worker->enqueue(request(23, ports::TwitchStartAuthentication{})).has_value());
    MANNY_CHECK(suite, worker->pending_count() == 1);
    worker->cancel_pending();
    MANNY_CHECK(suite, worker->is_stopping());
    MANNY_CHECK(suite, worker->pending_count() == 0);
    MANNY_CHECK(suite, worker->result_count() == 0);
    MANNY_CHECK(suite,
                !worker->enqueue(request(24, ports::TwitchStartAuthentication{})).has_value());
    worker.reset();
    MANNY_CHECK(suite, client.saw_stop());
}

} // namespace

void run_twitch_authentication_worker_tests(TestSuite& suite) {
    operation_and_success_tests(suite);
    failure_retention_and_exception_tests(suite);
    backpressure_and_cancellation_tests(suite);
}

} // namespace manny_uploader::test
