#include "manny_uploader/providers/donbot_provider_worker.hpp"

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
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] providers::DonBotProviderConfig
config(std::string guild = "123456789012345678",
       std::string base = "https://donbot-api.walmslo.com") {
    return providers::DonBotProviderConfig{
        .api_base_url = std::move(base),
        .guild_id = std::move(guild),
        .discord_delivery_mode = domain::DonBotDiscordDeliveryMode::None,
        .discord_channel_id = {},
    };
}

[[nodiscard]] providers::DonBotError
upload_error(providers::DonBotDisposition disposition, std::string detail,
             std::optional<std::chrono::seconds> retry_after = std::nullopt) {
    return providers::DonBotError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = std::nullopt,
        .http_status = std::nullopt,
    };
}

[[nodiscard]] ports::UploadRequest request(std::uint64_t id,
                                           domain::Provider provider = domain::Provider::DonBot) {
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

struct CapturedUpload {
    std::string api_base_url;
    std::string guild_id;
    std::string api_key;
    std::optional<std::string> dps_report_permalink;
    providers::DonBotDiscordDeliveryRequest discord_delivery;
};

class FakeDonBotClient final : public providers::IDonBotClient {
  public:
    using Result = std::expected<providers::DonBotUploadSuccess, providers::DonBotError>;

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
        return condition_.wait_for(lock, timeout,
                                   [this, count] { return uploads_.size() >= count; });
    }

    [[nodiscard]] std::vector<CapturedUpload> uploads() const {
        const std::scoped_lock lock{mutex_};
        return uploads_;
    }

    [[nodiscard]] bool saw_stop() const {
        const std::scoped_lock lock{mutex_};
        return saw_stop_;
    }

    [[nodiscard]] std::expected<providers::DonBotVerification, providers::DonBotError>
    verify(std::string_view, const support::SecretValue&, const std::stop_token&) const override {
        return std::unexpected(upload_error(providers::DonBotDisposition::Failed, "unused"));
    }

    [[nodiscard]] std::expected<providers::DonBotUploadSuccess, providers::DonBotError>
    upload(const domain::LogFileIdentity&, std::string_view api_base_url, std::string_view guild_id,
           const support::SecretValue& api_key,
           const providers::DonBotDiscordDeliveryRequest& discord_delivery,
           const std::stop_token& stop_token) const override {
        return run(api_base_url, guild_id, api_key, std::nullopt, discord_delivery, stop_token);
    }

    [[nodiscard]] std::expected<providers::DonBotUploadSuccess, providers::DonBotError>
    import_permalink(std::string_view permalink, std::string_view api_base_url,
                     std::string_view guild_id, const support::SecretValue& api_key,
                     const providers::DonBotDiscordDeliveryRequest& discord_delivery,
                     const std::stop_token& stop_token) const override {
        return run(api_base_url, guild_id, api_key, std::string{permalink}, discord_delivery,
                   stop_token);
    }

  private:
    [[nodiscard]] Result run(std::string_view api_base_url, std::string_view guild_id,
                             const support::SecretValue& api_key,
                             std::optional<std::string> permalink,
                             const providers::DonBotDiscordDeliveryRequest& discord_delivery,
                             const std::stop_token& stop_token) const {
        std::stop_callback wake_on_stop{stop_token, [this] { condition_.notify_all(); }};
        std::unique_lock lock{mutex_};
        uploads_.push_back(CapturedUpload{
            .api_base_url = std::string{api_base_url},
            .guild_id = std::string{guild_id},
            .api_key = secret_text(api_key),
            .dps_report_permalink = std::move(permalink),
            .discord_delivery = discord_delivery,
        });
        condition_.notify_all();
        condition_.wait(lock, [this, &stop_token] {
            return !blocked_ || released_ || stop_token.stop_requested();
        });
        if (stop_token.stop_requested()) {
            saw_stop_ = true;
            return std::unexpected(
                upload_error(providers::DonBotDisposition::Cancelled, "cancelled"));
        }
        if (throw_next_) {
            throw_next_ = false;
            throw std::runtime_error{"private DonBot client exception"};
        }
        if (results_.empty()) {
            return providers::DonBotUploadSuccess{
                .upload_id = std::nullopt,
                .fight_log_id = std::nullopt,
                .discord_delivery = {},
            };
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::deque<Result> results_;
    mutable std::vector<CapturedUpload> uploads_;
    mutable bool blocked_{};
    mutable bool released_{};
    mutable bool throw_next_{};
    mutable bool saw_stop_{};
};

class FakeSecretStore final : public ports::ISecretStore {
  public:
    enum class Behavior : std::uint8_t {
        Value,
        NotFound,
        Fail,
        Throw,
    };

    explicit FakeSecretStore(std::string value = "GW2-API-KEY") : value_{std::move(value)} {}

    void set_behavior(Behavior behavior) {
        const std::scoped_lock lock{mutex_};
        behavior_ = behavior;
    }

    [[nodiscard]] std::size_t load_calls() const {
        const std::scoped_lock lock{mutex_};
        return load_calls_;
    }

    [[nodiscard]] bool used_expected_id() const {
        const std::scoped_lock lock{mutex_};
        return last_id_ == ports::SecretId::DonBotGw2ApiKey;
    }

    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        const std::scoped_lock lock{mutex_};
        ++load_calls_;
        last_id_ = id;
        if (behavior_ == Behavior::Throw) {
            throw std::runtime_error{"private secret store exception"};
        }
        if (behavior_ == Behavior::NotFound) {
            return std::unexpected(error(ports::SecretStoreErrorCode::NotFound));
        }
        if (behavior_ == Behavior::Fail) {
            return std::unexpected(error(ports::SecretStoreErrorCode::ReadFailed));
        }
        return support::SecretValue::from_text(value_);
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId, const support::SecretValue&) override {
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId) override {
        return {};
    }

  private:
    [[nodiscard]] static ports::SecretStoreError error(ports::SecretStoreErrorCode code) {
        return ports::SecretStoreError{
            .code = code,
            .id = ports::SecretId::DonBotGw2ApiKey,
            .message = "private secret error",
            .system_error = std::nullopt,
        };
    }

    mutable std::mutex mutex_;
    std::string value_;
    Behavior behavior_{Behavior::Value};
    mutable std::size_t load_calls_{};
    mutable ports::SecretId last_id_{ports::SecretId::DpsReportUserToken};
};

void creation_and_outcome_tests(TestSuite& suite) {
    FakeDonBotClient client;
    FakeSecretStore secrets{"VERY-SECRET-KEY"};
    const auto zero_capacity =
        providers::DonBotProviderWorker::create(client, secrets, config(), 0);
    MANNY_CHECK(suite, !zero_capacity.has_value());
    MANNY_CHECK(suite, zero_capacity.error().code ==
                           providers::DonBotProviderWorkerErrorCode::InvalidCapacity);
    const auto bad_config =
        providers::DonBotProviderWorker::create(client, secrets, config("not-a-guild"));
    MANNY_CHECK(suite, !bad_config.has_value());
    MANNY_CHECK(suite, bad_config.error().code ==
                           providers::DonBotProviderWorkerErrorCode::InvalidConfiguration);

    auto dormant_config = config();
    dormant_config.guild_id.clear();
    auto dormant = providers::DonBotProviderWorker::create(client, secrets, dormant_config);
    MANNY_CHECK(suite, dormant.has_value());
    MANNY_CHECK(suite, !(*dormant)->enqueue(request(10)).has_value());

    client.push(providers::DonBotUploadSuccess{
        .upload_id = std::uint64_t{91},
        .fight_log_id = std::uint64_t{191},
        .discord_delivery = {},
    });
    client.push(
        std::unexpected(upload_error(providers::DonBotDisposition::Retry, "retry detail", 12s)));
    client.push(
        std::unexpected(upload_error(providers::DonBotDisposition::Retry, "default retry")));
    client.push(
        std::unexpected(upload_error(providers::DonBotDisposition::Failed, "failed detail")));
    client.push(
        std::unexpected(upload_error(providers::DonBotDisposition::Cancelled, "cancelled detail")));
    auto created = providers::DonBotProviderWorker::create(client, secrets, config(), 5);
    MANNY_CHECK(suite, created.has_value());
    auto worker = std::move(*created);
    MANNY_CHECK(suite, worker->provider() == domain::Provider::DonBot);
    MANNY_CHECK(suite, worker->config_snapshot() == config());

    for (std::uint64_t id = 11; id <= 15; ++id) {
        MANNY_CHECK(suite, worker->enqueue(request(id)).has_value());
        const auto result = worker->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        if (!result) {
            continue;
        }
        MANNY_CHECK(suite, result->job_id == domain::UploadJobId{id});
        MANNY_CHECK(suite, result->provider == domain::Provider::DonBot);
        MANNY_CHECK(suite, !result->dps_report_result.has_value());
        MANNY_CHECK(suite, result->detail.find("VERY-SECRET-KEY") == std::string::npos);
        if (id == 11) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Succeeded);
            MANNY_CHECK(suite, result->detail == "Uploaded and processed by DonBot (fight 191)");
            MANNY_CHECK(suite, result->donbot_upload_receipt.has_value());
            MANNY_CHECK(suite, result->donbot_upload_receipt &&
                                   result->donbot_upload_receipt->upload_id == 91);
            MANNY_CHECK(suite, result->donbot_upload_receipt &&
                                   result->donbot_upload_receipt->fight_log_id == 191);
        } else if (id == 12) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Retry);
            MANNY_CHECK(suite, result->retry_after == 12s);
        } else if (id == 13) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Retry);
            MANNY_CHECK(suite, result->retry_after == 30s);
        } else if (id == 14) {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Failed);
        } else {
            MANNY_CHECK(suite, result->outcome == ports::UploadOutcome::Cancelled);
        }
    }
    const auto uploads = client.uploads();
    MANNY_CHECK(suite, uploads.size() == 5);
    MANNY_CHECK(suite, !uploads.empty() && uploads.front().api_key == "VERY-SECRET-KEY");
    MANNY_CHECK(suite, secrets.load_calls() == 5);
    MANNY_CHECK(suite, secrets.used_expected_id());
}

void credential_and_validation_tests(TestSuite& suite) {
    for (const auto behavior :
         {FakeSecretStore::Behavior::NotFound, FakeSecretStore::Behavior::Fail,
          FakeSecretStore::Behavior::Throw}) {
        FakeDonBotClient client;
        FakeSecretStore secrets;
        secrets.set_behavior(behavior);
        auto worker = providers::DonBotProviderWorker::create(client, secrets, config());
        MANNY_CHECK(suite, worker.has_value());
        MANNY_CHECK(suite, (*worker)->enqueue(request(21)).has_value());
        const auto result = (*worker)->wait_for_result(2s);
        MANNY_CHECK(suite, result.has_value());
        MANNY_CHECK(suite, result && result->outcome == ports::UploadOutcome::Failed);
        MANNY_CHECK(suite, result && result->detail.find("private") == std::string::npos);
        MANNY_CHECK(suite, client.uploads().empty());
    }

    FakeDonBotClient client;
    FakeSecretStore secrets;
    auto worker = providers::DonBotProviderWorker::create(client, secrets, config());
    MANNY_CHECK(suite, worker.has_value());
    auto wrong_provider = request(31, domain::Provider::Wingman);
    auto has_report = request(32);
    has_report.dps_report_result = domain::DpsReportResult{
        .permalink = "https://dps.report/donbot-import",
        .encounter_name = "Boss",
        .boss_id = 123,
        .mode = "CM",
        .success = true,
    };
    auto has_context = request(33);
    has_context.donbot_context = ports::DonBotUploadContext{
        .api_base_url = "https://example.com",
        .guild_id = "1",
        .discord_delivery_mode = domain::DonBotDiscordDeliveryMode::None,
        .discord_channel_id = {},
    };
    auto empty_path = request(34);
    empty_path.file.canonical_path.clear();
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(wrong_provider)).has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(std::move(has_report)).has_value());
    MANNY_CHECK(suite, (*worker)->wait_for_result(2s).has_value());
    const auto imports = client.uploads();
    MANNY_CHECK(suite, imports.size() == 1);
    MANNY_CHECK(suite, imports.front().dps_report_permalink ==
                           std::optional<std::string>{"https://dps.report/donbot-import"});
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(has_context)).has_value());
    MANNY_CHECK(suite, !(*worker)->enqueue(std::move(empty_path)).has_value());
    MANNY_CHECK(suite, !(*worker)->update_config(config("0")).has_value());
}

void configuration_snapshot_tests(TestSuite& suite) {
    FakeDonBotClient client;
    client.block();
    FakeSecretStore secrets;
    auto worker = providers::DonBotProviderWorker::create(client, secrets, config(), 3);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(41)).has_value());
    MANNY_CHECK(suite, client.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(42)).has_value());
    auto updated = config("223456789012345678", "https://new-donbot.example/root/");
    updated.discord_delivery_mode = domain::DonBotDiscordDeliveryMode::ChannelOverride;
    updated.discord_channel_id = "323456789012345678";
    MANNY_CHECK(suite, (*worker)->update_config(updated).has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(43)).has_value());
    client.release();

    for (std::size_t index = 0; index < 3; ++index) {
        MANNY_CHECK(suite, (*worker)->wait_for_result(2s).has_value());
    }
    const auto uploads = client.uploads();
    MANNY_CHECK(suite, uploads.size() == 3);
    if (uploads.size() == 3) {
        MANNY_CHECK(suite, uploads[0].guild_id == "123456789012345678");
        MANNY_CHECK(suite, uploads[1].guild_id == "123456789012345678");
        MANNY_CHECK(suite, uploads[2].guild_id == "223456789012345678");
        MANNY_CHECK(suite, uploads[2].api_base_url == "https://new-donbot.example/root/");
        MANNY_CHECK(suite,
                    uploads[0].discord_delivery.mode == domain::DonBotDiscordDeliveryMode::None);
        MANNY_CHECK(suite,
                    uploads[1].discord_delivery.mode == domain::DonBotDiscordDeliveryMode::None);
        MANNY_CHECK(suite, uploads[2].discord_delivery.mode ==
                               domain::DonBotDiscordDeliveryMode::ChannelOverride);
        MANNY_CHECK(suite, uploads[2].discord_delivery.channel_id == "323456789012345678");
    }

    auto reupload = request(44);
    reupload.user_initiated_retry = true;
    MANNY_CHECK(suite, (*worker)->enqueue(std::move(reupload)).has_value());
    MANNY_CHECK(suite, (*worker)->wait_for_result(2s).has_value());
    const auto after_reupload = client.uploads();
    MANNY_CHECK(suite, after_reupload.size() == 4);
    MANNY_CHECK(suite, after_reupload.size() == 4 && after_reupload.back().discord_delivery.mode ==
                                                         domain::DonBotDiscordDeliveryMode::None);
    MANNY_CHECK(suite, after_reupload.size() == 4 &&
                           after_reupload.back().discord_delivery.channel_id.empty());
}

void cancellation_and_exception_tests(TestSuite& suite) {
    FakeDonBotClient throwing;
    throwing.throw_next();
    FakeSecretStore secrets;
    auto throwing_worker = providers::DonBotProviderWorker::create(throwing, secrets, config());
    MANNY_CHECK(suite, throwing_worker.has_value());
    MANNY_CHECK(suite, (*throwing_worker)->enqueue(request(51)).has_value());
    const auto thrown = (*throwing_worker)->wait_for_result(2s);
    MANNY_CHECK(suite, thrown.has_value());
    MANNY_CHECK(suite, thrown && thrown->outcome == ports::UploadOutcome::Failed);
    MANNY_CHECK(suite, thrown && thrown->detail.find("private") == std::string::npos);

    FakeDonBotClient blocked;
    blocked.block();
    auto worker = providers::DonBotProviderWorker::create(blocked, secrets, config(), 2);
    MANNY_CHECK(suite, worker.has_value());
    MANNY_CHECK(suite, (*worker)->enqueue(request(52)).has_value());
    MANNY_CHECK(suite, blocked.wait_for_calls(1, 2s));
    MANNY_CHECK(suite, (*worker)->enqueue(request(53)).has_value());
    (*worker)->cancel_pending();
    MANNY_CHECK(suite, (*worker)->is_stopping());
    MANNY_CHECK(suite, (*worker)->pending_count() == 0);
    MANNY_CHECK(suite, !(*worker)->enqueue(request(54)).has_value());
    worker->reset();
    MANNY_CHECK(suite, blocked.saw_stop());
    MANNY_CHECK(suite, blocked.uploads().size() == 1);
}

} // namespace

void run_donbot_provider_worker_tests(TestSuite& suite) {
    creation_and_outcome_tests(suite);
    credential_and_validation_tests(suite);
    configuration_snapshot_tests(suite);
    cancellation_and_exception_tests(suite);
}

} // namespace manny_uploader::test
