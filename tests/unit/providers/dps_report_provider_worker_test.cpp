#include "manny_uploader/providers/dps_report_provider_worker.hpp"

#include "support/test_suite.hpp"

#include <atomic>
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
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;
using providers::DpsReportProviderWorker;
using providers::DpsReportProviderWorkerErrorCode;

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] domain::DpsReportResult report(std::string suffix = "worker") {
    return domain::DpsReportResult{
        .permalink = "https://dps.report/" + std::move(suffix),
        .encounter_name = "Worker Boss",
        .boss_id = 123,
        .mode = "CM",
        .success = true,
    };
}

[[nodiscard]] providers::DpsReportUploadSuccess
success(std::optional<support::SecretValue> replacement = std::nullopt,
        std::optional<std::string> warning = std::nullopt, std::string suffix = "worker") {
    return providers::DpsReportUploadSuccess{
        .report = report(std::move(suffix)),
        .replacement_user_token = std::move(replacement),
        .warning = std::move(warning),
    };
}

[[nodiscard]] providers::DpsReportUploadError
upload_error(providers::DpsReportUploadDisposition disposition, std::string detail,
             std::optional<std::chrono::seconds> retry_after = std::nullopt) {
    return providers::DpsReportUploadError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = std::nullopt,
        .http_status = std::nullopt,
    };
}

[[nodiscard]] ports::UploadRequest
request(std::uint64_t id, domain::Provider provider = domain::Provider::DpsReport) {
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
        .dps_report_context = std::nullopt,
        .donbot_context = std::nullopt,
        .attempt = 1,
    };
}

class FakeDpsReportClient final : public providers::IDpsReportClient {
  public:
    using Result =
        std::expected<providers::DpsReportUploadSuccess, providers::DpsReportUploadError>;

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

    [[nodiscard]] bool saw_token(std::string_view expected) const {
        const std::scoped_lock lock{mutex_};
        return received_token_ == expected;
    }

    [[nodiscard]] bool saw_no_token() const {
        const std::scoped_lock lock{mutex_};
        return !received_token_.has_value();
    }

    [[nodiscard]] bool saw_stop() const {
        const std::scoped_lock lock{mutex_};
        return saw_stop_;
    }

    [[nodiscard]] std::vector<bool> detailed_wvw_options() const {
        const std::scoped_lock lock{mutex_};
        return detailed_wvw_options_;
    }

    [[nodiscard]] std::expected<providers::DpsReportUploadSuccess, providers::DpsReportUploadError>
    upload(const domain::LogFileIdentity&, const support::SecretValue* user_token,
           const std::stop_token& stop_token,
           providers::DpsReportUploadOptions options) const override {
        std::stop_callback wake_on_stop{stop_token, [this] { condition_.notify_all(); }};
        std::unique_lock lock{mutex_};
        ++calls_;
        received_token_ = user_token == nullptr
                              ? std::nullopt
                              : std::optional<std::string>{secret_text(*user_token)};
        detailed_wvw_options_.push_back(options.detailed_wvw);
        condition_.notify_all();
        condition_.wait(lock, [this, &stop_token] {
            return !blocked_ || released_ || stop_token.stop_requested();
        });
        if (stop_token.stop_requested()) {
            saw_stop_ = true;
            return std::unexpected(
                upload_error(providers::DpsReportUploadDisposition::Cancelled, "cancelled"));
        }
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error{"private client exception marker"};
        }
        if (results_.empty()) {
            return success();
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
    mutable std::optional<std::string> received_token_;
    mutable std::vector<bool> detailed_wvw_options_;
    mutable bool blocked_{};
    mutable bool released_{};
    mutable bool throw_next_{};
    mutable bool saw_stop_{};
};

class FakeSecretStore final : public ports::ISecretStore {
  public:
    enum class LoadBehavior : std::uint8_t {
        Value,
        NotFound,
        Fail,
        Throw,
    };

    enum class StoreBehavior : std::uint8_t {
        Succeed,
        Fail,
        Throw,
    };

    explicit FakeSecretStore(std::string token)
        : token_{std::move(token)}, load_behavior_{LoadBehavior::Value} {}

    FakeSecretStore() = default;

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        const std::scoped_lock lock{mutex_};
        ++load_calls_;
        last_load_id_ = id;
        if (load_behavior_ == LoadBehavior::Throw) {
            throw std::runtime_error{"private load exception marker"};
        }
        if (load_behavior_ == LoadBehavior::NotFound) {
            return std::unexpected(
                error(ports::SecretStoreErrorCode::NotFound, "private missing detail"));
        }
        if (load_behavior_ == LoadBehavior::Fail) {
            return std::unexpected(
                error(ports::SecretStoreErrorCode::ReadFailed, "private load failure detail"));
        }
        return support::SecretValue::from_text(token_);
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId id, const support::SecretValue& value) override {
        const std::scoped_lock lock{mutex_};
        ++store_calls_;
        last_store_id_ = id;
        if (store_behavior_ == StoreBehavior::Throw) {
            throw std::runtime_error{"private store exception marker"};
        }
        if (store_behavior_ == StoreBehavior::Fail) {
            return std::unexpected(
                error(ports::SecretStoreErrorCode::ReplaceFailed, "private store failure detail"));
        }
        token_ = secret_text(value);
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId) override {
        return {};
    }

    void set_load_behavior(LoadBehavior behavior) {
        const std::scoped_lock lock{mutex_};
        load_behavior_ = behavior;
    }

    void set_store_behavior(StoreBehavior behavior) {
        const std::scoped_lock lock{mutex_};
        store_behavior_ = behavior;
    }

    [[nodiscard]] std::string token() const {
        const std::scoped_lock lock{mutex_};
        return token_;
    }

    [[nodiscard]] std::size_t load_calls() const {
        const std::scoped_lock lock{mutex_};
        return load_calls_;
    }

    [[nodiscard]] std::size_t store_calls() const {
        const std::scoped_lock lock{mutex_};
        return store_calls_;
    }

    [[nodiscard]] bool used_expected_ids() const {
        const std::scoped_lock lock{mutex_};
        return last_load_id_ == ports::SecretId::DpsReportUserToken &&
               last_store_id_ == ports::SecretId::DpsReportUserToken;
    }

  private:
    [[nodiscard]] static ports::SecretStoreError error(ports::SecretStoreErrorCode code,
                                                       std::string message) {
        return ports::SecretStoreError{
            .code = code,
            .id = ports::SecretId::DpsReportUserToken,
            .message = std::move(message),
            .system_error = std::nullopt,
        };
    }

    mutable std::mutex mutex_;
    std::string token_;
    LoadBehavior load_behavior_{LoadBehavior::NotFound};
    StoreBehavior store_behavior_{StoreBehavior::Succeed};
    mutable std::size_t load_calls_{};
    std::size_t store_calls_{};
    mutable ports::SecretId last_load_id_{ports::SecretId::DonBotGw2ApiKey};
    ports::SecretId last_store_id_{ports::SecretId::DonBotGw2ApiKey};
};

void creation_and_success_tests(TestSuite& suite) {
    FakeDpsReportClient client;
    const auto invalid = DpsReportProviderWorker::create(client, nullptr, 0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == DpsReportProviderWorkerErrorCode::InvalidCapacity);

    FakeSecretStore secrets{"old-token"};
    client.push(success(support::SecretValue::from_text("new-token")));
    auto created = DpsReportProviderWorker::create(client, &secrets, 2);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(*created);
    MANNY_CHECK(suite, worker->provider() == domain::Provider::DpsReport);
    MANNY_CHECK(suite, !worker->is_stopping());
    MANNY_CHECK(suite, !worker->try_take_result().has_value());
    MANNY_CHECK(suite, worker->enqueue(request(17)).has_value());

    const auto result = worker->wait_for_result(2s);
    MANNY_CHECK(suite, result.has_value());
    if (result) {
        MANNY_CHECK(suite, result->job_id == domain::UploadJobId{17});
        MANNY_CHECK(suite, result->provider == domain::Provider::DpsReport);
        MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Succeeded);
        MANNY_CHECK(suite, !result->retry_after.has_value());
        MANNY_CHECK(suite, result->dps_report_result.has_value());
        MANNY_CHECK(suite, result->dps_report_result->boss_id == 123);
        MANNY_CHECK(suite, result->detail == "Uploaded to dps.report");
        MANNY_CHECK(suite, result->detail.find("old-token") == std::string::npos);
        MANNY_CHECK(suite, result->detail.find("new-token") == std::string::npos);
    }
    MANNY_CHECK(suite, client.saw_token("old-token"));
    MANNY_CHECK(suite, secrets.token() == "new-token");
    MANNY_CHECK(suite, secrets.load_calls() == 1);
    MANNY_CHECK(suite, secrets.store_calls() == 1);
    MANNY_CHECK(suite, secrets.used_expected_ids());
    MANNY_CHECK(suite, worker->pending_count() == 0);
    MANNY_CHECK(suite, worker->result_count() == 0);
}

void optional_storage_tests(TestSuite& suite) {
    FakeDpsReportClient anonymous;
    anonymous.push(success(support::SecretValue::from_text("discarded-token")));
    auto anonymous_worker = DpsReportProviderWorker::create(anonymous);
    MANNY_CHECK(suite, anonymous_worker.has_value());
    MANNY_CHECK(suite, (*anonymous_worker)->enqueue(request(21)).has_value());
    const auto anonymous_result = (*anonymous_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, anonymous_result.has_value());
    MANNY_CHECK(suite, anonymous.saw_no_token());
    MANNY_CHECK(suite,
                anonymous_result && anonymous_result->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, anonymous_result && anonymous_result->detail.find(
                                               "storage is unavailable") != std::string::npos);
    MANNY_CHECK(suite, anonymous_result &&
                           anonymous_result->detail.find("discarded-token") == std::string::npos);

    FakeDpsReportClient missing_client;
    FakeSecretStore missing_store;
    missing_store.set_load_behavior(FakeSecretStore::LoadBehavior::NotFound);
    auto missing_worker = DpsReportProviderWorker::create(missing_client, &missing_store);
    MANNY_CHECK(suite, missing_worker.has_value());
    MANNY_CHECK(suite, (*missing_worker)->enqueue(request(22)).has_value());
    const auto missing_result = (*missing_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, missing_result.has_value());
    MANNY_CHECK(suite,
                missing_result && missing_result->outcome == ports::UploadOutcome::Succeeded);
    MANNY_CHECK(suite, missing_client.saw_no_token());
    MANNY_CHECK(suite, missing_store.store_calls() == 0);
}

void credential_failure_tests(TestSuite& suite) {
    for (const auto behavior :
         {FakeSecretStore::LoadBehavior::Fail, FakeSecretStore::LoadBehavior::Throw}) {
        FakeDpsReportClient client;
        FakeSecretStore secrets;
        secrets.set_load_behavior(behavior);
        auto worker = DpsReportProviderWorker::create(client, &secrets);
        MANNY_CHECK(suite, worker.has_value());
        MANNY_CHECK(suite, (*worker)->enqueue(request(31)).has_value());
        const auto result = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        MANNY_CHECK(suite, result && result->outcome == ports::UploadOutcome::Failed);
        MANNY_CHECK(suite, result && !result->dps_report_result.has_value());
        MANNY_CHECK(suite, result && result->detail.find("private") == std::string::npos);
        MANNY_CHECK(suite, client.calls() == 0);
    }

    for (const auto behavior :
         {FakeSecretStore::StoreBehavior::Fail, FakeSecretStore::StoreBehavior::Throw}) {
        FakeDpsReportClient client;
        client.push(success(support::SecretValue::from_text("replacement")));
        FakeSecretStore secrets{"current"};
        secrets.set_store_behavior(behavior);
        auto worker = DpsReportProviderWorker::create(client, &secrets);
        MANNY_CHECK(suite, worker.has_value());
        MANNY_CHECK(suite, (*worker)->enqueue(request(32)).has_value());
        const auto result = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        MANNY_CHECK(suite, result && result->outcome == ports::UploadOutcome::Failed);
        MANNY_CHECK(suite, result && !result->dps_report_result.has_value());
        MANNY_CHECK(suite, result && result->detail.find("private") == std::string::npos);
        MANNY_CHECK(suite, secrets.token() == "current");
    }
}

void outcome_mapping_tests(TestSuite& suite) {
    FakeDpsReportClient client;
    client.push(std::unexpected(
        upload_error(providers::DpsReportUploadDisposition::Retry, "retry detail", 12s)));
    client.push(std::unexpected(
        upload_error(providers::DpsReportUploadDisposition::Retry, "default retry detail")));
    client.push(std::unexpected(
        upload_error(providers::DpsReportUploadDisposition::Failed, "failed detail")));
    client.push(std::unexpected(
        upload_error(providers::DpsReportUploadDisposition::Cancelled, "cancelled detail")));
    client.push(success(std::nullopt, "generic warning", "warning"));
    auto worker = DpsReportProviderWorker::create(client, nullptr, 5);
    MANNY_CHECK(suite, worker.has_value());
    for (std::uint64_t id = 41; id <= 45; ++id) {
        MANNY_CHECK(suite, (*worker)->enqueue(request(id)).has_value());
        const auto result = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        if (!result) {
            continue;
        }
        if (id == 41) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Retry);
            MANNY_CHECK(suite, result->retry_after == 12s);
        } else if (id == 42) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Retry);
            MANNY_CHECK(suite, result->retry_after == 30s);
        } else if (id == 43) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Failed);
            MANNY_CHECK(suite, !result->retry_after.has_value());
        } else if (id == 44) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Cancelled);
            MANNY_CHECK(suite, !result->retry_after.has_value());
        } else {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Succeeded);
            MANNY_CHECK(suite, result->detail == "generic warning");
        }
    }

    FakeDpsReportClient throwing;
    throwing.throw_next();
    auto throwing_worker = DpsReportProviderWorker::create(throwing);
    MANNY_CHECK(suite, throwing_worker.has_value());
    MANNY_CHECK(suite, (*throwing_worker)->enqueue(request(46)).has_value());
    const auto thrown = (*throwing_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, thrown.has_value());
    MANNY_CHECK(suite, thrown && thrown->outcome == ports::UploadOutcome::Failed);
    MANNY_CHECK(suite, thrown && thrown->detail.find("private client exception marker") ==
                                     std::string::npos);
}

void request_validation_tests(TestSuite& suite) {
    FakeDpsReportClient client;
    auto worker = DpsReportProviderWorker::create(client);
    MANNY_CHECK(suite, worker.has_value());

    auto zero_job = request(51);
    zero_job.job_id = {};
    auto wrong_provider = request(52, domain::Provider::Wingman);
    auto empty_path = request(53);
    empty_path.file.canonical_path.clear();
    auto zero_attempt = request(54);
    zero_attempt.attempt = 0;
    auto has_report = request(55);
    has_report.dps_report_result = report();
    auto has_dps_report_context = request(56);
    has_dps_report_context.dps_report_context = ports::DpsReportUploadContext{.detailed_wvw = true};
    auto has_donbot_context = request(57);
    has_donbot_context.donbot_context = ports::DonBotUploadContext{
        .api_base_url = "https://donbot.example",
        .guild_id = "1",
        .discord_delivery_mode = domain::DonBotDiscordDeliveryMode::None,
        .discord_channel_id = {},
    };

    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(zero_job)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(wrong_provider)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(empty_path)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(zero_attempt)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(has_report)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(has_dps_report_context)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(has_donbot_context)).has_value());
    MANNY_CHECK(suite, client.calls() == 0);
}

void configuration_capture_tests(TestSuite& suite) {
    FakeDpsReportClient client;
    client.block();
    auto worker = DpsReportProviderWorker::create(
        client, nullptr, 2, 1, providers::DpsReportProviderConfig{.detailed_wvw = false});
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->config_snapshot() ==
                           providers::DpsReportProviderConfig{.detailed_wvw = false});
    MANNY_CHECK(suite, (*worker)->enqueue(request(58)).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));

    (*worker)->update_config(providers::DpsReportProviderConfig{.detailed_wvw = true});
    MANNY_CHECK(suite, (*worker)->enqueue(request(59)).has_value());
    (*worker)->update_config(providers::DpsReportProviderConfig{.detailed_wvw = false});
    client.release();

    MANNY_CHECK(suite, (*worker)->wait_for_result(2s).has_value());
    MANNY_CHECK(suite, (*worker)->wait_for_result(2s).has_value());
    const auto options = client.detailed_wvw_options();
    MANNY_CHECK(suite, options == std::vector<bool>({false, true}));
}

void bounded_queue_and_backpressure_tests(TestSuite& suite) {
    FakeDpsReportClient client;
    client.block();
    auto worker = DpsReportProviderWorker::create(client, nullptr, 1);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(61)).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(62)).has_value());
    MANNY_CHECK(suite, (*worker)->pending_count() == 1);
    const auto full = (*worker)->enqueue(request(63));
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().message.find("full") != std::string::npos);
    client.release();

    MANNY_CHECK(suite, client.wait_for_calls(2, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(63)).has_value());
    MANNY_CHECK(suite, !client.wait_for_calls(3, 50ms));
    const auto first = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, client.wait_for_calls(3, 2s));
    const auto second = (*worker)->wait_for_result(2s);
    const auto third = (*worker)->wait_for_result(2s);
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, third.has_value());
    MANNY_CHECK(suite, first && first->job_id == domain::UploadJobId{61});
    MANNY_CHECK(suite, second && second->job_id == domain::UploadJobId{62});
    MANNY_CHECK(suite, third && third->job_id == domain::UploadJobId{63});
    MANNY_CHECK(suite, (*worker)->pending_count() == 0);
    MANNY_CHECK(suite, (*worker)->result_count() == 0);
}

void cancellation_and_shutdown_tests(TestSuite& suite) {
    FakeDpsReportClient client;
    client.block();
    auto worker = DpsReportProviderWorker::create(client, nullptr, 2);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(71)).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(72)).has_value());

    (*worker)->cancel_pending();
    (*worker)->cancel_pending();
    MANNY_CHECK(suite, (*worker)->is_stopping());
    MANNY_CHECK(suite, (*worker)->pending_count() == 0);
    MANNY_CHECK(suite, (*worker)->result_count() == 0);
    MANNY_CHECK(suite, !(*worker)->enqueue(request(73)).has_value());
    MANNY_CHECK(suite, !(*worker)->wait_for_result(1ms).has_value());

    worker->reset();
    MANNY_CHECK(suite, client.saw_stop());
    MANNY_CHECK(suite, client.calls() == 1);
}

} // namespace

void run_dps_report_provider_worker_tests(TestSuite& suite) {
    creation_and_success_tests(suite);
    optional_storage_tests(suite);
    credential_failure_tests(suite);
    outcome_mapping_tests(suite);
    request_validation_tests(suite);
    configuration_capture_tests(suite);
    bounded_queue_and_backpressure_tests(suite);
    cancellation_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
