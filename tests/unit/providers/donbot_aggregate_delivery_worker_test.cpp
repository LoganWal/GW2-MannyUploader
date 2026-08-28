#include "manny_uploader/providers/donbot_aggregate_delivery_worker.hpp"

#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] providers::DonBotError
error(providers::DonBotDisposition disposition, std::string detail,
      std::optional<ports::HttpErrorCode> http_error = std::nullopt,
      std::optional<std::uint16_t> http_status = std::nullopt) {
    return providers::DonBotError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = std::nullopt,
        .http_error = http_error,
        .http_status = http_status,
    };
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct CapturedAggregate {
    std::vector<std::uint64_t> fight_log_ids;
    std::string api_base_url;
    std::string guild_id;
    std::string api_key;
    providers::DonBotDiscordDeliveryRequest delivery;
};

class FakeClient final : public providers::IDonBotClient {
  public:
    using Result = std::expected<providers::DonBotAggregateDeliverySuccess, providers::DonBotError>;

    void push(Result result) {
        const std::scoped_lock lock{mutex_};
        results_.push_back(std::move(result));
    }

    [[nodiscard]] std::vector<CapturedAggregate> calls() const {
        const std::scoped_lock lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::expected<providers::DonBotVerification, providers::DonBotError>
    verify(std::string_view, const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(error(providers::DonBotDisposition::Failed, "unused"));
    }

    [[nodiscard]] std::expected<providers::DonBotUploadSuccess, providers::DonBotError>
    upload(const domain::LogFileIdentity&, std::string_view, std::string_view,
           const support::SecretValue&, const providers::DonBotDiscordDeliveryRequest&,
           const std::stop_token&) const override {
        return std::unexpected(error(providers::DonBotDisposition::Failed, "unused"));
    }

    [[nodiscard]] std::expected<providers::DonBotUploadSuccess, providers::DonBotError>
    import_permalink(std::string_view, std::string_view, std::string_view,
                     const support::SecretValue&, const providers::DonBotDiscordDeliveryRequest&,
                     const std::stop_token&) const override {
        return std::unexpected(error(providers::DonBotDisposition::Failed, "unused"));
    }

    [[nodiscard]] Result deliver_aggregate(std::span<const std::uint64_t> fight_log_ids,
                                           std::string_view api_base_url, std::string_view guild_id,
                                           const support::SecretValue& api_key,
                                           const providers::DonBotDiscordDeliveryRequest& delivery,
                                           const std::stop_token&) const override {
        const std::scoped_lock lock{mutex_};
        calls_.push_back(CapturedAggregate{
            .fight_log_ids = {fight_log_ids.begin(), fight_log_ids.end()},
            .api_base_url = std::string{api_base_url},
            .guild_id = std::string{guild_id},
            .api_key = secret_text(api_key),
            .delivery = delivery,
        });
        if (results_.empty()) {
            return std::unexpected(error(providers::DonBotDisposition::Failed, "missing result"));
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::deque<Result> results_;
    mutable std::vector<CapturedAggregate> calls_;
};

class FakeSecretStore final : public ports::ISecretStore {
  public:
    bool available{true};

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        if (!available || id != ports::SecretId::DonBotGw2ApiKey) {
            return std::unexpected(ports::SecretStoreError{
                .code = ports::SecretStoreErrorCode::NotFound,
                .id = id,
                .message = "private secret failure",
                .system_error = std::nullopt,
            });
        }
        return support::SecretValue::from_text("AGGREGATE-KEY");
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId, const support::SecretValue&) override {
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId) override {
        return {};
    }
};

[[nodiscard]] ports::DonBotAggregateDeliveryRequest request(std::uint64_t request_id) {
    return ports::DonBotAggregateDeliveryRequest{
        .request_id = request_id,
        .api_base_url = "https://donbot.example",
        .guild_id = "123",
        .fight_log_ids = {101, 202},
        .delivery_mode = domain::DonBotDiscordDeliveryMode::ChannelOverride,
        .channel_id = "223",
    };
}

[[nodiscard]] std::optional<ports::DonBotAggregateDeliveryResult>
wait_for_result(providers::DonBotAggregateDeliveryWorker& worker) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.try_take_result()) {
            return result;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

void success_and_backpressure_tests(TestSuite& suite) {
    FakeClient client;
    client.push(providers::DonBotAggregateDeliverySuccess{
        .fight_log_count = 2,
        .discord_delivery =
            domain::DonBotDiscordDeliveryReceipt{
                .outcome = domain::DonBotDiscordDeliveryOutcome::Sent,
                .sent = 2,
            },
    });
    FakeSecretStore secrets;
    auto created = providers::DonBotAggregateDeliveryWorker::create(client, secrets);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(*created);
    MANNY_CHECK(suite, worker->enqueue(request(1)).has_value());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (worker->busy() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    MANNY_CHECK(suite, !worker->enqueue(request(2)).has_value());
    const auto result = wait_for_result(*worker);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result && result->request_id == 1);
    MANNY_CHECK(suite,
                result && result->outcome == ports::DonBotAggregateDeliveryOutcome::Succeeded);
    MANNY_CHECK(suite, result && result->fight_log_count == 2);
    const auto calls = client.calls();
    MANNY_CHECK(suite, calls.size() == 1);
    if (!calls.empty()) {
        MANNY_CHECK(suite, calls.front().fight_log_ids == std::vector<std::uint64_t>({101, 202}));
        MANNY_CHECK(suite, calls.front().guild_id == "123");
        MANNY_CHECK(suite, calls.front().api_key == "AGGREGATE-KEY");
        MANNY_CHECK(suite, calls.front().delivery.mode ==
                               domain::DonBotDiscordDeliveryMode::ChannelOverride);
        MANNY_CHECK(suite, calls.front().delivery.channel_id == "223");
    }
}

void receipt_outcome_tests(TestSuite& suite) {
    struct Case {
        domain::DonBotDiscordDeliveryReceipt receipt;
        ports::DonBotAggregateDeliveryOutcome expected;
        std::string_view detail;
    };
    constexpr std::array cases{
        Case{
            .receipt =
                domain::DonBotDiscordDeliveryReceipt{
                    .outcome = domain::DonBotDiscordDeliveryOutcome::Sent,
                    .sent = 2,
                },
            .expected = ports::DonBotAggregateDeliveryOutcome::Succeeded,
            .detail = "2 sent, 0 skipped, 0 failed, 0 ambiguous",
        },
        Case{
            .receipt =
                domain::DonBotDiscordDeliveryReceipt{
                    .outcome = domain::DonBotDiscordDeliveryOutcome::Partial,
                    .sent = 1,
                    .failed = 1,
                },
            .expected = ports::DonBotAggregateDeliveryOutcome::Succeeded,
            .detail = "1 sent, 0 skipped, 1 failed, 0 ambiguous",
        },
        Case{
            .receipt =
                domain::DonBotDiscordDeliveryReceipt{
                    .outcome = domain::DonBotDiscordDeliveryOutcome::Skipped,
                    .skipped = 2,
                },
            .expected = ports::DonBotAggregateDeliveryOutcome::Failed,
            .detail = "0 sent, 2 skipped, 0 failed, 0 ambiguous",
        },
        Case{
            .receipt =
                domain::DonBotDiscordDeliveryReceipt{
                    .outcome = domain::DonBotDiscordDeliveryOutcome::Failed,
                    .failed = 2,
                },
            .expected = ports::DonBotAggregateDeliveryOutcome::Failed,
            .detail = "0 sent, 0 skipped, 2 failed, 0 ambiguous",
        },
        Case{
            .receipt =
                domain::DonBotDiscordDeliveryReceipt{
                    .outcome = domain::DonBotDiscordDeliveryOutcome::Ambiguous,
                    .ambiguous = 2,
                },
            .expected = ports::DonBotAggregateDeliveryOutcome::Ambiguous,
            .detail = "0 sent, 0 skipped, 0 failed, 2 ambiguous",
        },
    };

    std::uint64_t request_id = 20;
    for (const auto& test_case : cases) {
        FakeClient client;
        client.push(providers::DonBotAggregateDeliverySuccess{
            .fight_log_count = 2,
            .discord_delivery = test_case.receipt,
        });
        FakeSecretStore secrets;
        auto created = providers::DonBotAggregateDeliveryWorker::create(client, secrets);
        MANNY_CHECK(suite, created.has_value());
        MANNY_CHECK(suite, (*created)->enqueue(request(request_id++)).has_value());
        const auto result = wait_for_result(**created);
        MANNY_CHECK(suite, result.has_value());
        MANNY_CHECK(suite, result && result->outcome == test_case.expected);
        MANNY_CHECK(suite, result && result->discord_delivery == test_case.receipt);
        MANNY_CHECK(suite, result && result->detail.contains(test_case.detail));
    }

    FakeClient partial_ambiguous_client;
    partial_ambiguous_client.push(providers::DonBotAggregateDeliverySuccess{
        .fight_log_count = 2,
        .discord_delivery =
            domain::DonBotDiscordDeliveryReceipt{
                .outcome = domain::DonBotDiscordDeliveryOutcome::Partial,
                .sent = 1,
                .ambiguous = 1,
            },
    });
    FakeSecretStore secrets;
    auto partial_ambiguous =
        providers::DonBotAggregateDeliveryWorker::create(partial_ambiguous_client, secrets);
    MANNY_CHECK(suite, partial_ambiguous.has_value());
    MANNY_CHECK(suite, (*partial_ambiguous)->enqueue(request(request_id)).has_value());
    const auto result = wait_for_result(**partial_ambiguous);
    MANNY_CHECK(suite,
                result && result->outcome == ports::DonBotAggregateDeliveryOutcome::Ambiguous);
}

void failure_policy_tests(TestSuite& suite) {
    FakeClient client;
    client.push(std::unexpected(error(providers::DonBotDisposition::Retry, "private response",
                                      std::nullopt, std::uint16_t{503})));
    FakeSecretStore secrets;
    auto created = providers::DonBotAggregateDeliveryWorker::create(client, secrets);
    MANNY_CHECK(suite, created.has_value());
    MANNY_CHECK(suite, (*created)->enqueue(request(11)).has_value());
    const auto ambiguous = wait_for_result(**created);
    MANNY_CHECK(suite, ambiguous.has_value());
    MANNY_CHECK(suite, ambiguous &&
                           ambiguous->outcome == ports::DonBotAggregateDeliveryOutcome::Ambiguous);
    MANNY_CHECK(suite, ambiguous && ambiguous->detail.find("private") == std::string::npos);
    MANNY_CHECK(suite, client.calls().size() == 1);

    FakeClient cancelled_client;
    cancelled_client.push(
        std::unexpected(error(providers::DonBotDisposition::Cancelled, "private cancellation")));
    auto cancelled = providers::DonBotAggregateDeliveryWorker::create(cancelled_client, secrets);
    MANNY_CHECK(suite, cancelled.has_value());
    MANNY_CHECK(suite, (*cancelled)->enqueue(request(13)).has_value());
    const auto cancelled_result = wait_for_result(**cancelled);
    MANNY_CHECK(suite, cancelled_result.has_value());
    MANNY_CHECK(suite, cancelled_result && cancelled_result->outcome ==
                                               ports::DonBotAggregateDeliveryOutcome::Ambiguous);
    MANNY_CHECK(suite,
                cancelled_result && cancelled_result->detail.find("private") == std::string::npos);

    FakeClient unused_client;
    FakeSecretStore missing_secrets;
    missing_secrets.available = false;
    auto missing = providers::DonBotAggregateDeliveryWorker::create(unused_client, missing_secrets);
    MANNY_CHECK(suite, missing.has_value());
    MANNY_CHECK(suite, (*missing)->enqueue(request(12)).has_value());
    const auto failed = wait_for_result(**missing);
    MANNY_CHECK(suite, failed.has_value());
    MANNY_CHECK(suite, failed && failed->outcome == ports::DonBotAggregateDeliveryOutcome::Failed);
    MANNY_CHECK(suite, unused_client.calls().empty());

    auto invalid = request(0);
    MANNY_CHECK(suite, !(*missing)->enqueue(std::move(invalid)).has_value());
}

} // namespace

void run_donbot_aggregate_delivery_worker_tests(TestSuite& suite) {
    success_and_backpressure_tests(suite);
    receipt_outcome_tests(suite);
    failure_policy_tests(suite);
}

} // namespace manny_uploader::test
