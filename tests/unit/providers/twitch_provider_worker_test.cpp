#include "manny_uploader/providers/twitch_provider_worker.hpp"
#include "support/test_suite.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
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
        {
            std::unique_lock lock{mutex};
            user_ids.emplace_back(user_id);
            messages.emplace_back(message);
            access_tokens.push_back(secret_text(access_token));
            entered = true;
            condition.notify_all();
            condition.wait(lock,
                           [this, &stop_token] { return !block || stop_token.stop_requested(); });
        }
        if (stop_token.stop_requested()) {
            return std::unexpected(client_error(providers::TwitchDisposition::Cancelled,
                                                "Twitch chat delivery was cancelled",
                                                ports::HttpErrorCode::Cancelled));
        }
        if (outcomes.empty()) {
            return std::unexpected(client_error(providers::TwitchDisposition::Failed));
        }
        auto outcome = std::move(outcomes.front());
        outcomes.pop_front();
        return outcome;
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
        rejected_tokens.push_back(secret_text(rejected.access_token));
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
            .authenticated_user_id = "123456",
            .revision = revision,
        };
    }

    mutable std::deque<Outcome> acquire_outcomes;
    mutable std::deque<Outcome> recover_outcomes;
    mutable std::size_t acquire_count{};
    mutable std::size_t recover_count{};
    mutable std::vector<std::uint64_t> rejected_revisions;
    mutable std::vector<std::string> rejected_tokens;
};

[[nodiscard]] providers::TwitchProviderConfig
config(std::string message_template = "{result}: {url}", bool post_success = true,
       bool post_failure = true) {
    return providers::TwitchProviderConfig{
        .message_template = std::move(message_template),
        .post_success = post_success,
        .post_failure = post_failure,
    };
}

[[nodiscard]] domain::DpsReportResult report(bool success = true,
                                             std::string permalink = "https://dps.report/one") {
    return domain::DpsReportResult{
        .permalink = std::move(permalink),
        .encounter_name = "Cerus",
        .boss_id = 26257,
        .mode = "LCM",
        .success = success,
    };
}

[[nodiscard]] ports::UploadRequest request(std::uint64_t id = 1, bool success = true,
                                           std::uint32_t attempt = 1,
                                           std::string permalink = "https://dps.report/one") {
    return ports::UploadRequest{
        .job_id = domain::UploadJobId{id},
        .provider = domain::Provider::Twitch,
        .file =
            domain::LogFileIdentity{
                .canonical_path = std::filesystem::path{"logs/test.zevtc"},
                .size = 4096,
                .last_write_time = {},
            },
        .metadata = domain::EncounterMetadata{.boss_id = 26257, .pov_account = "Player.1234"},
        .dps_report_result = report(success, std::move(permalink)),
        .donbot_context = std::nullopt,
        .twitch_context = std::nullopt,
        .attempt = attempt,
    };
}

[[nodiscard]] providers::TwitchChatResult sent(std::string id) {
    return providers::TwitchChatResult{
        .is_sent = true,
        .message_id = std::move(id),
        .drop_reason = std::nullopt,
    };
}

[[nodiscard]] providers::TwitchChatResult dropped(std::string code) {
    return providers::TwitchChatResult{
        .is_sent = false,
        .message_id = std::nullopt,
        .drop_reason =
            providers::TwitchDropReason{
                .code = std::move(code),
                .message = "server detail must not be published",
            },
    };
}

void construction_and_validation_tests(TestSuite& suite) {
    FakeTwitchClient client;
    FakeSessionAccess sessions;
    auto zero = providers::TwitchProviderWorker::create(client, sessions, config(), 0);
    MANNY_CHECK(suite, !zero.has_value());
    MANNY_CHECK(suite,
                zero.error().code == providers::TwitchProviderWorkerErrorCode::InvalidCapacity);

    auto no_policy =
        providers::TwitchProviderWorker::create(client, sessions, config("{url}", false, false));
    MANNY_CHECK(suite, !no_policy.has_value());
    auto bad_template =
        providers::TwitchProviderWorker::create(client, sessions, config("{unknown} {url}"));
    MANNY_CHECK(suite, !bad_template.has_value());

    auto created = providers::TwitchProviderWorker::create(client, sessions, config());
    MANNY_CHECK(suite, created.has_value());
    MANNY_CHECK(suite, (*created)->provider() == domain::Provider::Twitch);
    MANNY_CHECK(suite, (*created)->config_snapshot() == config());
    MANNY_CHECK(suite, !(*created)->update_config(config("{url}", false, false)));
    MANNY_CHECK(suite, (*created)->update_config(config("{encounter}: {url}")).has_value());

    auto missing = request();
    missing.dps_report_result.reset();
    MANNY_CHECK(suite, !(*created)->enqueue(std::move(missing)).has_value());
    auto wrong_context = request();
    wrong_context.donbot_context = ports::DonBotUploadContext{
        .api_base_url = "https://donbot.example",
        .guild_id = "1",
    };
    MANNY_CHECK(suite, !(*created)->enqueue(std::move(wrong_context)).has_value());
}

void successful_delivery_and_policy_tests(TestSuite& suite) {
    FakeTwitchClient client;
    client.outcomes.emplace_back(sent("message-1"));
    FakeSessionAccess sessions;
    sessions.acquire_outcomes.emplace_back(FakeSessionAccess::make_session("token-1", 7));
    auto worker = providers::TwitchProviderWorker::create(
        client, sessions, config("{encounter}{mode_suffix} — {result}: {url}"));
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request()).has_value());
    const auto result = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, result->twitch_delivery_receipt.has_value());
    MANNY_CHECK(suite,
                result->twitch_delivery_receipt->status == domain::TwitchDeliveryStatus::Sent);
    MANNY_CHECK(suite, result->twitch_delivery_receipt->message_id == "message-1");
    MANNY_CHECK(suite, client.user_ids == std::vector<std::string>{"123456"});
    MANNY_CHECK(suite, client.access_tokens == std::vector<std::string>{"token-1"});
    MANNY_CHECK(suite, client.messages == std::vector<std::string>{
                                              "Cerus (LCM) — Success: https://dps.report/one"});

    FakeTwitchClient skipped_client;
    FakeSessionAccess skipped_sessions;
    auto skipped = providers::TwitchProviderWorker::create(skipped_client, skipped_sessions,
                                                           config("{url}", false, true));
    MANNY_CHECK(suite, skipped.has_value());
    MANNY_CHECK(suite, (*skipped)->enqueue(request(2, true)).has_value());
    const auto skipped_result = (*skipped)->wait_for_result(2s);
    MANNY_CHECK(suite, skipped_result.has_value());
    MANNY_CHECK(suite, skipped_result->outcome == ports::UploadOutcome::Skipped);
    MANNY_CHECK(suite, skipped_sessions.acquire_count == 0);
    MANNY_CHECK(suite, skipped_client.messages.empty());
}

void configuration_snapshot_test(TestSuite& suite) {
    FakeTwitchClient client;
    client.block = true;
    client.outcomes.emplace_back(sent("first"));
    FakeSessionAccess sessions;
    auto worker = providers::TwitchProviderWorker::create(client, sessions, config("old {url}"));
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request()).has_value());
    client.wait_until_entered();
    MANNY_CHECK(suite, (*worker)->update_config(config("new {url}")).has_value());
    client.release();
    const auto result = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, result && result->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, client.messages == std::vector<std::string>{"old https://dps.report/one"});
}

void drop_reason_tests(TestSuite& suite) {
    struct DropCase {
        std::string code;
        domain::TwitchDeliveryStatus status;
    };
    const DropCase cases[]{
        {.code = "automod_held", .status = domain::TwitchDeliveryStatus::AutoMod},
        {.code = "automod_caught", .status = domain::TwitchDeliveryStatus::AutoMod},
        {.code = "blocked_term", .status = domain::TwitchDeliveryStatus::BlockedTerm},
        {.code = "msg_duplicate", .status = domain::TwitchDeliveryStatus::Duplicate},
        {.code = "rate_limited", .status = domain::TwitchDeliveryStatus::RateLimited},
        {.code = "followers_only", .status = domain::TwitchDeliveryStatus::FollowersOnly},
        {.code = "slow_mode", .status = domain::TwitchDeliveryStatus::SlowMode},
        {.code = "subscribers_only", .status = domain::TwitchDeliveryStatus::SubscribersOnly},
        {.code = "restricted", .status = domain::TwitchDeliveryStatus::Restricted},
        {.code = "new_future_reason", .status = domain::TwitchDeliveryStatus::OtherDrop},
    };
    for (std::size_t index = 0; index < std::size(cases); ++index) {
        FakeTwitchClient client;
        client.outcomes.emplace_back(dropped(cases[index].code));
        FakeSessionAccess sessions;
        auto worker = providers::TwitchProviderWorker::create(client, sessions, config());
        MANNY_CHECK(suite, worker.has_value());
        MANNY_CHECK(suite, (*worker)->enqueue(request(index + 10)).has_value());
        const auto result = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Failed);
        MANNY_CHECK(suite, result->twitch_delivery_receipt.has_value());
        MANNY_CHECK(suite, result->twitch_delivery_receipt->status == cases[index].status);
        MANNY_CHECK(suite, !result->twitch_delivery_receipt->message_id.has_value());
        MANNY_CHECK(suite, result->detail.find("server detail") == std::string::npos);
    }
}

void recovery_tests(TestSuite& suite) {
    FakeTwitchClient client;
    client.outcomes.emplace_back(std::unexpected(
        client_error(providers::TwitchDisposition::Reconnect, "reconnect", std::nullopt, 401)));
    client.outcomes.emplace_back(sent("after-refresh"));
    FakeSessionAccess sessions;
    sessions.acquire_outcomes.emplace_back(FakeSessionAccess::make_session("old", 9));
    sessions.recover_outcomes.emplace_back(FakeSessionAccess::make_session("new", 10));
    auto worker = providers::TwitchProviderWorker::create(client, sessions, config());
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request()).has_value());
    const auto result = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, result && result->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, sessions.acquire_count == 1);
    MANNY_CHECK(suite, sessions.recover_count == 1);
    MANNY_CHECK(suite, sessions.rejected_revisions == std::vector<std::uint64_t>{9});
    MANNY_CHECK(suite, sessions.rejected_tokens == std::vector<std::string>{"old"});
    MANNY_CHECK(suite, client.access_tokens == std::vector<std::string>({"old", "new"}));

    FakeTwitchClient twice_client;
    twice_client.outcomes.emplace_back(std::unexpected(
        client_error(providers::TwitchDisposition::Reconnect, "first", std::nullopt, 401)));
    twice_client.outcomes.emplace_back(std::unexpected(
        client_error(providers::TwitchDisposition::Reconnect, "second", std::nullopt, 401)));
    FakeSessionAccess twice_sessions;
    auto twice = providers::TwitchProviderWorker::create(twice_client, twice_sessions, config());
    MANNY_CHECK(suite, twice.has_value());
    MANNY_CHECK(suite, (*twice)->enqueue(request(2)).has_value());
    const auto twice_result = (*twice)->wait_for_result(2s);
    MANNY_CHECK(suite, twice_result && twice_result->outcome == ports::UploadOutcome::Failed);
    MANNY_CHECK(suite, twice_sessions.recover_count == 1);
    MANNY_CHECK(suite, twice_client.messages.size() == 2);

    FakeTwitchClient recovery_failure_client;
    recovery_failure_client.outcomes.emplace_back(std::unexpected(
        client_error(providers::TwitchDisposition::Reconnect, "reconnect", std::nullopt, 401)));
    FakeSessionAccess recovery_failure_sessions;
    recovery_failure_sessions.recover_outcomes.emplace_back(
        std::unexpected(ports::TwitchDeliverySessionError{
            .code = ports::TwitchDeliverySessionErrorCode::Retry,
            .detail = "Twitch session recovery is temporarily unavailable",
            .retry_after = 17s,
        }));
    auto recovery_failure = providers::TwitchProviderWorker::create(
        recovery_failure_client, recovery_failure_sessions, config());
    MANNY_CHECK(suite, recovery_failure.has_value());
    MANNY_CHECK(suite, (*recovery_failure)->enqueue(request(3)).has_value());
    const auto recovery_result = (*recovery_failure)->wait_for_result(2s);
    MANNY_CHECK(suite, recovery_result && recovery_result->outcome == ports::UploadOutcome::Retry);
    MANNY_CHECK(suite, recovery_result->retry_after == 17s);
    MANNY_CHECK(suite, recovery_failure_client.messages.size() == 1);
}

void retry_and_ambiguity_tests(TestSuite& suite) {
    struct SafeCase {
        std::optional<ports::HttpErrorCode> http_error;
        std::optional<std::uint16_t> status;
    };
    const SafeCase safe_cases[]{
        {.http_error = ports::HttpErrorCode::NameResolutionFailed, .status = std::nullopt},
        {.http_error = ports::HttpErrorCode::ConnectionFailed, .status = std::nullopt},
        {.http_error = ports::HttpErrorCode::TlsFailed, .status = std::nullopt},
        {.http_error = std::nullopt, .status = 429},
    };
    for (std::size_t index = 0; index < std::size(safe_cases); ++index) {
        FakeTwitchClient client;
        client.outcomes.emplace_back(std::unexpected(
            client_error(providers::TwitchDisposition::Retry, "safe retry",
                         safe_cases[index].http_error, safe_cases[index].status, 12s)));
        client.outcomes.emplace_back(sent("retried"));
        FakeSessionAccess sessions;
        auto worker = providers::TwitchProviderWorker::create(client, sessions, config());
        MANNY_CHECK(suite, worker.has_value());
        MANNY_CHECK(suite, (*worker)->enqueue(request(index + 20)).has_value());
        const auto first = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, first && first->outcome == ports::UploadOutcome::Retry);
        MANNY_CHECK(suite, first->retry_after == 12s);
        MANNY_CHECK(suite, (*worker)->enqueue(request(index + 20, true, 2)).has_value());
        const auto second = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, second && second->outcome == ports::UploadOutcome::Succeeded);
        MANNY_CHECK(suite, client.messages.size() == 2);
    }

    const providers::TwitchError ambiguous_cases[]{
        client_error(providers::TwitchDisposition::Retry, "timeout", ports::HttpErrorCode::Timeout),
        client_error(providers::TwitchDisposition::Retry, "partial send",
                     ports::HttpErrorCode::SendFailed),
        client_error(providers::TwitchDisposition::Retry, "receive failed",
                     ports::HttpErrorCode::ReceiveFailed),
        client_error(providers::TwitchDisposition::Retry, "server failed", std::nullopt, 500),
        client_error(providers::TwitchDisposition::Failed, "bad success", std::nullopt, 200),
    };
    for (std::size_t index = 0; index < std::size(ambiguous_cases); ++index) {
        FakeTwitchClient client;
        client.outcomes.emplace_back(std::unexpected(ambiguous_cases[index]));
        client.outcomes.emplace_back(sent("must-not-send"));
        FakeSessionAccess sessions;
        auto worker = providers::TwitchProviderWorker::create(client, sessions, config());
        MANNY_CHECK(suite, worker.has_value());
        MANNY_CHECK(suite, (*worker)->enqueue(request(index + 30)).has_value());
        const auto first = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, first && first->outcome == ports::UploadOutcome::Failed);
        MANNY_CHECK(suite, first->detail.find("suppressed") != std::string::npos);
        MANNY_CHECK(suite, (*worker)->enqueue(request(index + 30, true, 2)).has_value());
        const auto second = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, second && second->outcome == ports::UploadOutcome::Failed);
        MANNY_CHECK(suite, client.messages.size() == 1);
        MANNY_CHECK(suite, sessions.acquire_count == 1);
    }
}

void posted_dedupe_and_session_failure_tests(TestSuite& suite) {
    FakeTwitchClient client;
    client.outcomes.emplace_back(sent("only-once"));
    FakeSessionAccess sessions;
    auto worker = providers::TwitchProviderWorker::create(client, sessions, config());
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request()).has_value());
    const auto first = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, first && first->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, (*worker)->enqueue(request(1, true, 2)).has_value());
    const auto duplicate = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, duplicate && duplicate->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, duplicate->twitch_delivery_receipt->message_id == "only-once");
    MANNY_CHECK(suite, client.messages.size() == 1);
    MANNY_CHECK(suite, sessions.acquire_count == 1);

    FakeTwitchClient unavailable_client;
    FakeSessionAccess unavailable_sessions;
    unavailable_sessions.acquire_outcomes.emplace_back(
        std::unexpected(ports::TwitchDeliverySessionError{
            .code = ports::TwitchDeliverySessionErrorCode::NotConnected,
            .detail = "Connect Twitch before posting",
            .retry_after = std::nullopt,
        }));
    auto unavailable =
        providers::TwitchProviderWorker::create(unavailable_client, unavailable_sessions, config());
    MANNY_CHECK(suite, unavailable.has_value());
    MANNY_CHECK(suite, (*unavailable)->enqueue(request(2)).has_value());
    const auto unavailable_result = (*unavailable)->wait_for_result(2s);
    MANNY_CHECK(suite,
                unavailable_result && unavailable_result->outcome == ports::UploadOutcome::Failed);
    MANNY_CHECK(suite, unavailable_client.messages.empty());
}

void cancellation_test(TestSuite& suite) {
    FakeTwitchClient client;
    client.block = true;
    FakeSessionAccess sessions;
    auto worker = providers::TwitchProviderWorker::create(client, sessions, config());
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request()).has_value());
    client.wait_until_entered();
    (*worker)->cancel_pending();
    client.release();
    MANNY_CHECK(suite, (*worker)->is_stopping());
    MANNY_CHECK(suite, !(*worker)->enqueue(request(2)).has_value());
    MANNY_CHECK(suite, !(*worker)->wait_for_result(20ms).has_value());
}

} // namespace

void run_twitch_provider_worker_tests(TestSuite& suite) {
    construction_and_validation_tests(suite);
    successful_delivery_and_policy_tests(suite);
    configuration_snapshot_test(suite);
    drop_reason_tests(suite);
    recovery_tests(suite);
    retry_and_ambiguity_tests(suite);
    posted_dedupe_and_session_failure_tests(suite);
    cancellation_test(suite);
}

} // namespace manny_uploader::test
