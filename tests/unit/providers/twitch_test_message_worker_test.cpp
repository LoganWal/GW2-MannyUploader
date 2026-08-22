#include "manny_uploader/providers/twitch_test_message_worker.hpp"
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
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] support::SecretValue secret(std::string_view value) {
    return support::SecretValue::from_text(value);
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    return {reinterpret_cast<const char*>(value.bytes().data()), value.size()};
}

[[nodiscard]] providers::TwitchError
client_error(providers::TwitchDisposition disposition, std::string detail = "Twitch failed",
             std::optional<ports::HttpErrorCode> http_error = std::nullopt,
             std::optional<std::uint16_t> status = std::nullopt,
             std::optional<std::chrono::seconds> retry_after = std::nullopt) {
    return providers::TwitchError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = http_error,
        .http_status = status,
    };
}

[[nodiscard]] providers::TwitchChatResult sent(std::string id) {
    return providers::TwitchChatResult{
        .is_sent = true,
        .message_id = std::move(id),
        .drop_reason = std::nullopt,
    };
}

[[nodiscard]] providers::TwitchChatResult dropped(std::string code, std::string detail) {
    return providers::TwitchChatResult{
        .is_sent = false,
        .message_id = std::nullopt,
        .drop_reason =
            providers::TwitchDropReason{
                .code = std::move(code),
                .message = std::move(detail),
            },
    };
}

class FakeTwitchClient final : public providers::ITwitchClient {
  public:
    using ChatOutcome = std::expected<providers::TwitchChatResult, providers::TwitchError>;

    [[nodiscard]] std::expected<providers::TwitchDeviceAuthorization, providers::TwitchError>
    start_device_authorization(const std::stop_token&) const override {
        return std::unexpected(client_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] std::expected<providers::TwitchDevicePollResult, providers::TwitchError>
    poll_device_authorization(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(client_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] std::expected<providers::TwitchValidatedIdentity, providers::TwitchError>
    validate_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(client_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] std::expected<providers::TwitchTokenGrant, providers::TwitchError>
    refresh_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(client_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] std::expected<void, providers::TwitchError>
    revoke_access_token(const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(client_error(providers::TwitchDisposition::Failed));
    }

    [[nodiscard]] std::expected<providers::TwitchChatResult, providers::TwitchError>
    send_chat_message(std::string_view user_id, std::string_view message,
                      const support::SecretValue& access_token,
                      const std::stop_token& stop_token) const override {
        std::unique_lock lock{mutex};
        user_ids.emplace_back(user_id);
        messages.emplace_back(message);
        access_tokens.push_back(secret_text(access_token));
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this, &stop_token] { return !block || stop_token.stop_requested(); });
        if (stop_token.stop_requested()) {
            return std::unexpected(client_error(providers::TwitchDisposition::Cancelled,
                                                "Twitch chat delivery was cancelled",
                                                ports::HttpErrorCode::Cancelled));
        }
        if (throw_on_send) {
            throw std::runtime_error{"client failure"};
        }
        if (outcomes.empty()) {
            return std::unexpected(client_error(providers::TwitchDisposition::Failed));
        }
        auto result = std::move(outcomes.front());
        outcomes.pop_front();
        return result;
    }

    void wait_until_entered() const {
        std::unique_lock lock{mutex};
        condition.wait_for(lock, 2s, [this] { return entered; });
    }

    void release() const {
        const std::scoped_lock lock{mutex};
        block = false;
        condition.notify_all();
    }

    mutable std::deque<ChatOutcome> outcomes;
    mutable std::vector<std::string> user_ids;
    mutable std::vector<std::string> messages;
    mutable std::vector<std::string> access_tokens;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    mutable bool block{};
    mutable bool entered{};
    mutable bool throw_on_send{};
};

class FakeSessionAccess final : public ports::ITwitchDeliverySessionAccess {
  public:
    using Outcome = std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>;

    [[nodiscard]] Outcome acquire(const std::stop_token&) const override {
        ++acquire_count;
        if (!acquire_outcomes.empty()) {
            auto result = std::move(acquire_outcomes.front());
            acquire_outcomes.pop_front();
            return result;
        }
        return make_session("access", acquire_count);
    }

    [[nodiscard]] Outcome recover(ports::TwitchDeliverySession rejected,
                                  const std::stop_token&) const override {
        ++recover_count;
        rejected_revisions.push_back(rejected.revision);
        if (!recover_outcomes.empty()) {
            auto result = std::move(recover_outcomes.front());
            recover_outcomes.pop_front();
            return result;
        }
        return make_session("recovered", rejected.revision + 1);
    }

    [[nodiscard]] static ports::TwitchDeliverySession make_session(std::string token,
                                                                   std::uint64_t revision = 1) {
        return ports::TwitchDeliverySession{
            .access_token = secret(token),
            .authenticated_user_id = "broadcaster-id",
            .revision = revision,
        };
    }

    mutable std::deque<Outcome> acquire_outcomes;
    mutable std::deque<Outcome> recover_outcomes;
    mutable std::size_t acquire_count{};
    mutable std::size_t recover_count{};
    mutable std::vector<std::uint64_t> rejected_revisions;
};

void construction_and_success_tests(TestSuite& suite) {
    FakeTwitchClient client;
    FakeSessionAccess sessions;
    const auto zero = providers::TwitchTestMessageWorker::create(client, sessions, 0);
    MANNY_CHECK(suite, !zero.has_value());
    MANNY_CHECK(suite,
                zero.error().code == providers::TwitchTestMessageWorkerErrorCode::InvalidCapacity);
    MANNY_CHECK(suite,
                providers::make_twitch_test_message(42) == "GW2 Manny Uploader test message #42");

    client.outcomes.emplace_back(sent("test-message-id"));
    sessions.acquire_outcomes.emplace_back(FakeSessionAccess::make_session("token", 7));
    auto worker = providers::TwitchTestMessageWorker::create(client, sessions);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue({.request_id = 0}).has_value());
    MANNY_CHECK(suite, (*worker)->enqueue({.request_id = 42}).has_value());
    const auto result = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result->request_id == 42);
    MANNY_CHECK(suite, result->outcome == ports::TwitchTestMessageOutcome::Sent);
    MANNY_CHECK(suite, result->detail == "Twitch test message sent");
    MANNY_CHECK(suite, result->delivery_status == domain::TwitchDeliveryStatus::Sent);
    MANNY_CHECK(suite, !result->delivery_ambiguous);
    MANNY_CHECK(suite, client.user_ids == std::vector<std::string>{"broadcaster-id"});
    MANNY_CHECK(suite,
                client.messages == std::vector<std::string>{"GW2 Manny Uploader test message #42"});
    MANNY_CHECK(suite, client.access_tokens == std::vector<std::string>{"token"});
}

void drop_retry_and_ambiguity_tests(TestSuite& suite) {
    FakeTwitchClient dropped_client;
    dropped_client.outcomes.emplace_back(
        dropped("automod_held", "PRIVATE server moderation detail"));
    FakeSessionAccess dropped_sessions;
    auto dropped_worker =
        providers::TwitchTestMessageWorker::create(dropped_client, dropped_sessions);
    MANNY_CHECK(suite, dropped_worker.has_value());
    MANNY_CHECK(suite, (*dropped_worker)->enqueue({.request_id = 1}).has_value());
    const auto drop_result = (*dropped_worker)->wait_for_result(2s);
    MANNY_CHECK(suite,
                drop_result && drop_result->outcome == ports::TwitchTestMessageOutcome::Dropped);
    MANNY_CHECK(suite, drop_result->delivery_status == domain::TwitchDeliveryStatus::AutoMod);
    MANNY_CHECK(suite, drop_result->detail.find("PRIVATE") == std::string::npos);

    FakeTwitchClient retry_client;
    retry_client.outcomes.emplace_back(
        std::unexpected(client_error(providers::TwitchDisposition::Retry, "Network is unavailable",
                                     ports::HttpErrorCode::ConnectionFailed, std::nullopt, 12s)));
    FakeSessionAccess retry_sessions;
    auto retry_worker = providers::TwitchTestMessageWorker::create(retry_client, retry_sessions);
    MANNY_CHECK(suite, retry_worker.has_value());
    MANNY_CHECK(suite, (*retry_worker)->enqueue({.request_id = 2}).has_value());
    const auto retry_result = (*retry_worker)->wait_for_result(2s);
    MANNY_CHECK(suite,
                retry_result && retry_result->outcome == ports::TwitchTestMessageOutcome::Retry);
    MANNY_CHECK(suite, retry_result->retry_after == 12s);
    MANNY_CHECK(suite, retry_client.messages.size() == 1);

    FakeTwitchClient ambiguous_client;
    ambiguous_client.outcomes.emplace_back(
        std::unexpected(client_error(providers::TwitchDisposition::Retry, "PRIVATE timeout body",
                                     ports::HttpErrorCode::Timeout)));
    FakeSessionAccess ambiguous_sessions;
    auto ambiguous_worker =
        providers::TwitchTestMessageWorker::create(ambiguous_client, ambiguous_sessions);
    MANNY_CHECK(suite, ambiguous_worker.has_value());
    MANNY_CHECK(suite, (*ambiguous_worker)->enqueue({.request_id = 3}).has_value());
    const auto ambiguous = (*ambiguous_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, ambiguous && ambiguous->outcome == ports::TwitchTestMessageOutcome::Failed);
    MANNY_CHECK(suite, ambiguous->delivery_ambiguous);
    MANNY_CHECK(suite, ambiguous->detail.find("PRIVATE") == std::string::npos);
    MANNY_CHECK(suite, ambiguous_client.messages.size() == 1);
}

void recovery_and_session_tests(TestSuite& suite) {
    FakeTwitchClient client;
    client.outcomes.emplace_back(std::unexpected(client_error(
        providers::TwitchDisposition::Reconnect, "expired", std::nullopt, std::uint16_t{401})));
    client.outcomes.emplace_back(sent("after-recovery"));
    FakeSessionAccess sessions;
    sessions.acquire_outcomes.emplace_back(FakeSessionAccess::make_session("old", 9));
    sessions.recover_outcomes.emplace_back(FakeSessionAccess::make_session("new", 10));
    auto worker = providers::TwitchTestMessageWorker::create(client, sessions);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue({.request_id = 7}).has_value());
    const auto result = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, result && result->outcome == ports::TwitchTestMessageOutcome::Sent);
    MANNY_CHECK(suite, sessions.acquire_count == 1);
    MANNY_CHECK(suite, sessions.recover_count == 1);
    MANNY_CHECK(suite, sessions.rejected_revisions == std::vector<std::uint64_t>{9});
    MANNY_CHECK(suite, client.access_tokens == std::vector<std::string>({"old", "new"}));

    FakeTwitchClient twice_client;
    twice_client.outcomes.emplace_back(std::unexpected(client_error(
        providers::TwitchDisposition::Reconnect, "first", std::nullopt, std::uint16_t{401})));
    twice_client.outcomes.emplace_back(std::unexpected(client_error(
        providers::TwitchDisposition::Reconnect, "second", std::nullopt, std::uint16_t{401})));
    FakeSessionAccess twice_sessions;
    auto twice = providers::TwitchTestMessageWorker::create(twice_client, twice_sessions);
    MANNY_CHECK(suite, twice.has_value());
    MANNY_CHECK(suite, (*twice)->enqueue({.request_id = 8}).has_value());
    const auto twice_result = (*twice)->wait_for_result(2s);
    MANNY_CHECK(suite,
                twice_result && twice_result->outcome == ports::TwitchTestMessageOutcome::Failed);
    MANNY_CHECK(suite, twice_sessions.recover_count == 1);
    MANNY_CHECK(suite, twice_client.messages.size() == 2);

    FakeTwitchClient unavailable_client;
    FakeSessionAccess unavailable_sessions;
    unavailable_sessions.acquire_outcomes.emplace_back(
        std::unexpected(ports::TwitchDeliverySessionError{
            .code = ports::TwitchDeliverySessionErrorCode::NotConnected,
            .detail = "Connect Twitch before posting",
            .retry_after = std::nullopt,
        }));
    auto unavailable =
        providers::TwitchTestMessageWorker::create(unavailable_client, unavailable_sessions);
    MANNY_CHECK(suite, unavailable.has_value());
    MANNY_CHECK(suite, (*unavailable)->enqueue({.request_id = 9}).has_value());
    const auto unavailable_result = (*unavailable)->wait_for_result(2s);
    MANNY_CHECK(suite, unavailable_result &&
                           unavailable_result->outcome == ports::TwitchTestMessageOutcome::Failed);
    MANNY_CHECK(suite, unavailable_client.messages.empty());
}

void backpressure_exception_and_cancellation_tests(TestSuite& suite) {
    FakeTwitchClient client;
    client.block = true;
    client.outcomes.emplace_back(sent("first"));
    client.outcomes.emplace_back(sent("second"));
    FakeSessionAccess sessions;
    auto worker = providers::TwitchTestMessageWorker::create(client, sessions, 1);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue({.request_id = 1}).has_value());
    client.wait_until_entered();
    MANNY_CHECK(suite, (*worker)->enqueue({.request_id = 2}).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue({.request_id = 3}).has_value());
    client.release();
    const auto first = (*worker)->wait_for_result(2s);
    const auto second = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, first && first->request_id == 1);
    MANNY_CHECK(suite, second && second->request_id == 2);

    FakeTwitchClient throwing_client;
    throwing_client.throw_on_send = true;
    FakeSessionAccess throwing_sessions;
    auto throwing = providers::TwitchTestMessageWorker::create(throwing_client, throwing_sessions);
    MANNY_CHECK(suite, throwing.has_value());
    MANNY_CHECK(suite, (*throwing)->enqueue({.request_id = 10}).has_value());
    const auto failed = (*throwing)->wait_for_result(2s);
    MANNY_CHECK(suite, failed && failed->outcome == ports::TwitchTestMessageOutcome::Failed);
    MANNY_CHECK(suite, failed->detail == "The Twitch test message failed unexpectedly");

    FakeTwitchClient cancelled_client;
    cancelled_client.block = true;
    FakeSessionAccess cancelled_sessions;
    auto cancelled =
        providers::TwitchTestMessageWorker::create(cancelled_client, cancelled_sessions);
    MANNY_CHECK(suite, cancelled.has_value());
    MANNY_CHECK(suite, (*cancelled)->enqueue({.request_id = 11}).has_value());
    cancelled_client.wait_until_entered();
    (*cancelled)->cancel_pending();
    cancelled_client.release();
    MANNY_CHECK(suite, (*cancelled)->is_stopping());
    MANNY_CHECK(suite, !(*cancelled)->enqueue({.request_id = 12}).has_value());
    MANNY_CHECK(suite, !(*cancelled)->wait_for_result(20ms).has_value());
}

} // namespace

void run_twitch_test_message_worker_tests(TestSuite& suite) {
    construction_and_success_tests(suite);
    drop_retry_and_ambiguity_tests(suite);
    recovery_and_session_tests(suite);
    backpressure_exception_and_cancellation_tests(suite);
}

} // namespace manny_uploader::test
