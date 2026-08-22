#include "manny_uploader/providers/wingman_provider_worker.hpp"

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
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;
using providers::WingmanProviderWorker;
using providers::WingmanProviderWorkerErrorCode;

[[nodiscard]] providers::WingmanUploadError
upload_error(providers::WingmanUploadDisposition disposition, std::string detail,
             std::optional<std::chrono::seconds> retry_after = std::nullopt) {
    return providers::WingmanUploadError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = std::nullopt,
        .http_status = std::nullopt,
    };
}

[[nodiscard]] ports::UploadRequest request(std::uint64_t id,
                                           domain::Provider provider = domain::Provider::Wingman) {
    return ports::UploadRequest{
        .job_id = domain::UploadJobId{id},
        .provider = provider,
        .file =
            domain::LogFileIdentity{
                .canonical_path = std::filesystem::path{"logs"} / (std::to_string(id) + ".zevtc"),
                .size = 4096,
                .last_write_time = {},
            },
        .metadata = domain::EncounterMetadata{.boss_id = 123, .pov_account = "Player.1234"},
        .dps_report_result = std::nullopt,
        .donbot_context = std::nullopt,
        .attempt = 1,
    };
}

[[nodiscard]] domain::DpsReportResult report() {
    return domain::DpsReportResult{
        .permalink = "https://dps.report/not-for-wingman",
        .encounter_name = "Boss",
        .boss_id = 123,
        .mode = "",
        .success = true,
    };
}

class FakeWingmanClient final : public providers::IWingmanClient {
  public:
    using Result = std::expected<providers::WingmanUploadSuccess, providers::WingmanUploadError>;

    void push(Result result) {
        const std::scoped_lock lock{mutex_};
        results_.push_back(std::move(result));
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

    [[nodiscard]] std::size_t calls() const {
        const std::scoped_lock lock{mutex_};
        return calls_;
    }

    [[nodiscard]] bool saw_expected_metadata() const {
        const std::scoped_lock lock{mutex_};
        return boss_id_ == 123 && account_ == "Player.1234";
    }

    [[nodiscard]] bool saw_stop() const {
        const std::scoped_lock lock{mutex_};
        return saw_stop_;
    }

    [[nodiscard]] std::expected<providers::WingmanUploadSuccess, providers::WingmanUploadError>
    upload(const domain::LogFileIdentity&, const domain::EncounterMetadata& metadata,
           const std::stop_token& stop_token) const override {
        std::stop_callback wake_on_stop{stop_token, [this] { condition_.notify_all(); }};
        std::unique_lock lock{mutex_};
        ++calls_;
        boss_id_ = metadata.boss_id;
        account_ = metadata.pov_account;
        condition_.notify_all();
        condition_.wait(lock, [this, &stop_token] {
            return !blocked_ || released_ || stop_token.stop_requested();
        });
        if (stop_token.stop_requested()) {
            saw_stop_ = true;
            return std::unexpected(
                upload_error(providers::WingmanUploadDisposition::Cancelled, "cancelled"));
        }
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error{"private client exception"};
        }
        if (results_.empty()) {
            return providers::WingmanUploadSuccess{.duplicate = false};
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::deque<Result> results_;
    mutable std::size_t calls_{};
    mutable std::uint16_t boss_id_{};
    mutable std::string account_;
    mutable bool blocked_{};
    mutable bool released_{};
    mutable bool throw_next_{};
    mutable bool saw_stop_{};
};

class NoopProcessor final : public providers::IUploadRequestProcessor {
  public:
    [[nodiscard]] ports::UploadResult process(const ports::UploadRequest& request,
                                              const std::stop_token&) const override {
        return ports::UploadResult{
            .job_id = request.job_id,
            .provider = request.provider,
            .outcome = ports::UploadOutcome::Succeeded,
            .detail = {},
            .retry_after = std::nullopt,
            .dps_report_result = std::nullopt,
        };
    }
};

void creation_and_outcome_tests(TestSuite& suite) {
    NoopProcessor processor;
    const auto unknown = providers::AsyncUploadWorker::create(
        static_cast<domain::Provider>(domain::provider_count), processor, "failed");
    MANNY_CHECK(suite, !unknown.has_value());
    MANNY_CHECK(suite,
                unknown.error().code == providers::AsyncUploadWorkerErrorCode::InvalidProvider);
    const auto zero_capacity =
        providers::AsyncUploadWorker::create(domain::Provider::Wingman, processor, "failed", 0);
    MANNY_CHECK(suite, !zero_capacity.has_value());
    MANNY_CHECK(suite, zero_capacity.error().code ==
                           providers::AsyncUploadWorkerErrorCode::InvalidCapacity);

    FakeWingmanClient invalid_client;
    const auto invalid = WingmanProviderWorker::create(invalid_client, 0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == WingmanProviderWorkerErrorCode::InvalidCapacity);

    FakeWingmanClient client;
    client.push(providers::WingmanUploadSuccess{.duplicate = false});
    client.push(providers::WingmanUploadSuccess{.duplicate = true});
    client.push(std::unexpected(
        upload_error(providers::WingmanUploadDisposition::Retry, "retry detail", 12s)));
    client.push(std::unexpected(
        upload_error(providers::WingmanUploadDisposition::Retry, "default retry detail")));
    client.push(std::unexpected(
        upload_error(providers::WingmanUploadDisposition::Retry, "bounded retry detail", 25h)));
    client.push(std::unexpected(
        upload_error(providers::WingmanUploadDisposition::Failed, "failed detail")));
    client.push(std::unexpected(
        upload_error(providers::WingmanUploadDisposition::Cancelled, "cancelled detail")));

    auto created = WingmanProviderWorker::create(client, 7);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(*created);
    MANNY_CHECK(suite, worker->provider() == domain::Provider::Wingman);
    MANNY_CHECK(suite, !worker->is_stopping());
    MANNY_CHECK(suite, !worker->try_take_result().has_value());

    for (std::uint64_t id = 11; id <= 17; ++id) {
        MANNY_CHECK(suite, worker->enqueue(request(id)).has_value());
        const auto result = worker->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        if (!result) {
            continue;
        }
        MANNY_CHECK(suite, result->job_id == domain::UploadJobId{id});
        MANNY_CHECK(suite, result->provider == domain::Provider::Wingman);
        MANNY_CHECK(suite, !result->dps_report_result.has_value());
        if (id == 11) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Succeeded);
            MANNY_CHECK(suite, result->detail == "Uploaded to GW2Wingman");
        } else if (id == 12) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Succeeded);
            MANNY_CHECK(suite, result->detail == "Already present in GW2Wingman");
        } else if (id == 13) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Retry);
            MANNY_CHECK(suite, result->retry_after == 12s);
        } else if (id == 14 || id == 15) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Retry);
            MANNY_CHECK(suite, result->retry_after == 30s);
        } else if (id == 16) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Failed);
            MANNY_CHECK(suite, !result->retry_after.has_value());
        } else {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Cancelled);
            MANNY_CHECK(suite, !result->retry_after.has_value());
        }
    }
    MANNY_CHECK(suite, client.saw_expected_metadata());
    MANNY_CHECK(suite, worker->pending_count() == 0);
    MANNY_CHECK(suite, worker->result_count() == 0);

    FakeWingmanClient throwing;
    throwing.throw_next();
    auto throwing_worker = WingmanProviderWorker::create(throwing);
    MANNY_CHECK(suite, throwing_worker.has_value());
    MANNY_CHECK(suite, (*throwing_worker)->enqueue(request(18)).has_value());
    const auto thrown = (*throwing_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, thrown.has_value());
    MANNY_CHECK(suite, thrown && thrown->outcome == ports::UploadOutcome::Failed);
    MANNY_CHECK(suite, thrown && thrown->detail.find("private") == std::string::npos);
}

void request_validation_tests(TestSuite& suite) {
    FakeWingmanClient client;
    auto worker = WingmanProviderWorker::create(client);
    MANNY_CHECK(suite, worker.has_value());

    auto zero_job = request(21);
    zero_job.job_id = {};
    auto wrong_provider = request(22, domain::Provider::DpsReport);
    auto empty_path = request(23);
    empty_path.file.canonical_path.clear();
    auto zero_attempt = request(24);
    zero_attempt.attempt = 0;
    auto has_report = request(25);
    has_report.dps_report_result = report();
    auto has_donbot_context = request(26);
    has_donbot_context.donbot_context = ports::DonBotUploadContext{
        .api_base_url = "https://donbot.example",
        .guild_id = "1",
    };

    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(zero_job)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(wrong_provider)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(empty_path)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(zero_attempt)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(has_report)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(has_donbot_context)).has_value());
    MANNY_CHECK(suite, client.calls() == 0);
}

void bounded_queue_and_backpressure_tests(TestSuite& suite) {
    FakeWingmanClient client;
    client.block();
    auto worker = WingmanProviderWorker::create(client, 1);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(31)).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(32)).has_value());
    MANNY_CHECK(suite, (*worker)->pending_count() == 1);
    const auto full = (*worker)->enqueue(request(33));
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().message.find("full") != std::string::npos);
    client.release();

    MANNY_CHECK(suite, client.wait_for_calls(2, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(33)).has_value());
    MANNY_CHECK(suite, !client.wait_for_calls(3, 50ms));
    const auto first = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, client.wait_for_calls(3, 2s));
    const auto second = (*worker)->wait_for_result(2s);
    const auto third = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, third.has_value());
    MANNY_CHECK(suite, first && first->job_id == domain::UploadJobId{31});
    MANNY_CHECK(suite, second && second->job_id == domain::UploadJobId{32});
    MANNY_CHECK(suite, third && third->job_id == domain::UploadJobId{33});
}

void configurable_parallelism_tests(TestSuite& suite) {
    FakeWingmanClient client;
    client.block();
    auto worker = WingmanProviderWorker::create(client, 8, 3);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, worker && (*worker)->parallelism() == 3);

    for (std::uint64_t id = 51; id <= 55; ++id) {
        MANNY_CHECK(suite, (*worker)->enqueue(request(id)).has_value());
    }
    MANNY_CHECK(suite, client.wait_for_calls(3, 2s));
    MANNY_CHECK(suite, !client.wait_for_calls(4, 50ms));

    MANNY_CHECK(suite, (*worker)->update_parallelism(5).has_value());
    MANNY_CHECK(suite, (*worker)->parallelism() == 5);
    MANNY_CHECK(suite, client.wait_for_calls(5, 2s));

    const auto invalid = (*worker)->update_parallelism(0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite,
                invalid.error().code == providers::AsyncUploadWorkerErrorCode::InvalidCapacity);
    MANNY_CHECK(suite, (*worker)->parallelism() == 5);

    client.release();
    for (std::size_t index = 0; index < 5; ++index) {
        MANNY_CHECK(suite, (*worker)->wait_for_result(2s).has_value());
    }
}

void cancellation_and_shutdown_tests(TestSuite& suite) {
    FakeWingmanClient client;
    client.block();
    auto worker = WingmanProviderWorker::create(client, 2);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(41)).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(42)).has_value());

    (*worker)->cancel_pending();
    (*worker)->cancel_pending();
    MANNY_CHECK(suite, (*worker)->is_stopping());
    MANNY_CHECK(suite, (*worker)->pending_count() == 0);
    MANNY_CHECK(suite, (*worker)->result_count() == 0);
    MANNY_CHECK(suite, !(*worker)->enqueue(request(43)).has_value());
    MANNY_CHECK(suite, !(*worker)->wait_for_result(1ms).has_value());

    worker->reset();
    MANNY_CHECK(suite, client.saw_stop());
    MANNY_CHECK(suite, client.calls() == 1);
}

} // namespace

void run_wingman_provider_worker_tests(TestSuite& suite) {
    creation_and_outcome_tests(suite);
    request_validation_tests(suite);
    bounded_queue_and_backpressure_tests(suite);
    configurable_parallelism_tests(suite);
    cancellation_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
