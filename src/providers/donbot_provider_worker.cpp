#include "manny_uploader/providers/donbot_provider_worker.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace manny_uploader::providers {
namespace {

constexpr auto default_retry_delay = std::chrono::seconds{30};

[[nodiscard]] DonBotProviderWorkerError make_worker_error(DonBotProviderWorkerErrorCode code,
                                                          std::string message) {
    return DonBotProviderWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] bool valid_config(const DonBotProviderConfig& config) noexcept {
    if (!config.api_base_url.starts_with("https://") || config.api_base_url.size() > 2048 ||
        config.api_base_url.size() <= 8 || config.api_base_url.contains('@') ||
        config.api_base_url.contains('?') || config.api_base_url.contains('#') ||
        std::ranges::any_of(config.api_base_url, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte <= 0x20U || byte == 0x7fU;
        })) {
        return false;
    }
    constexpr std::size_t scheme_size = 8;
    const auto path_start = config.api_base_url.find('/', scheme_size);
    const auto authority =
        path_start == std::string::npos
            ? std::string_view{config.api_base_url}.substr(scheme_size)
            : std::string_view{config.api_base_url}.substr(scheme_size, path_start - scheme_size);
    if (authority.empty() || authority.contains('\\') ||
        config.api_base_url.find("/../", scheme_size) != std::string::npos ||
        config.api_base_url.ends_with("/..") ||
        config.api_base_url.find("/./", scheme_size) != std::string::npos ||
        config.api_base_url.ends_with("/.")) {
        return false;
    }
    if (config.guild_id.empty()) {
        return true;
    }
    std::uint64_t guild{};
    const auto parsed = std::from_chars(config.guild_id.data(),
                                        config.guild_id.data() + config.guild_id.size(), guild);
    return config.guild_id.size() <= 19 && config.guild_id.front() != '0' &&
           parsed.ec == std::errc{} &&
           parsed.ptr == config.guild_id.data() + config.guild_id.size() && guild > 0 &&
           guild <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] ports::UploadResult
make_result(domain::UploadJobId job_id, ports::UploadOutcome outcome, std::string detail,
            std::optional<std::chrono::seconds> retry_after = std::nullopt,
            std::optional<domain::DonBotUploadReceipt> receipt = std::nullopt) {
    return ports::UploadResult{
        .job_id = job_id,
        .provider = domain::Provider::DonBot,
        .outcome = outcome,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .dps_report_result = std::nullopt,
        .donbot_upload_receipt = std::move(receipt),
        .twitch_delivery_receipt = std::nullopt,
    };
}

[[nodiscard]] std::chrono::seconds
bounded_retry_delay(std::optional<std::chrono::seconds> delay) noexcept {
    if (!delay || *delay <= std::chrono::seconds::zero() || *delay > std::chrono::hours{24}) {
        return default_retry_delay;
    }
    return *delay;
}

} // namespace

std::expected<std::unique_ptr<DonBotProviderWorker>, DonBotProviderWorkerError>
DonBotProviderWorker::create(const IDonBotClient& client, const ports::ISecretStore& secret_store,
                             DonBotProviderConfig config, std::size_t queue_capacity,
                             std::size_t parallelism) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(DonBotProviderWorkerErrorCode::InvalidCapacity,
                              "DonBot provider queue capacity must be non-zero"));
    }
    if (!valid_config(config)) {
        return std::unexpected(
            make_worker_error(DonBotProviderWorkerErrorCode::InvalidConfiguration,
                              "DonBot provider configuration is invalid"));
    }
    try {
        auto provider = std::unique_ptr<DonBotProviderWorker>{
            new DonBotProviderWorker{client, secret_store, std::move(config)}};
        auto worker = AsyncUploadWorker::create(domain::Provider::DonBot, *provider,
                                                "The DonBot worker failed unexpectedly",
                                                queue_capacity, parallelism);
        if (!worker) {
            return std::unexpected(
                make_worker_error(DonBotProviderWorkerErrorCode::ThreadStartFailed,
                                  "Unable to start the DonBot provider worker"));
        }
        provider->worker_ = std::move(*worker);
        return provider;
    } catch (...) {
        return std::unexpected(make_worker_error(DonBotProviderWorkerErrorCode::ThreadStartFailed,
                                                 "Unable to start the DonBot provider worker"));
    }
}

DonBotProviderWorker::DonBotProviderWorker(const IDonBotClient& client,
                                           const ports::ISecretStore& secret_store,
                                           DonBotProviderConfig config)
    : client_{client}, secret_store_{secret_store}, config_{std::move(config)} {}

DonBotProviderWorker::~DonBotProviderWorker() {
    cancel_pending();
}

domain::Provider DonBotProviderWorker::provider() const noexcept {
    return domain::Provider::DonBot;
}

std::expected<void, ports::DispatchError>
DonBotProviderWorker::enqueue(ports::UploadRequest request) {
    if (request.dps_report_result || request.donbot_context || request.twitch_context) {
        return std::unexpected(ports::DispatchError{.message = "DonBot upload request is invalid"});
    }
    try {
        const auto snapshot = config_snapshot();
        if (snapshot.guild_id.empty()) {
            return std::unexpected(
                ports::DispatchError{.message = "The DonBot guild is not configured"});
        }
        request.donbot_context = ports::DonBotUploadContext{
            .api_base_url = snapshot.api_base_url,
            .guild_id = snapshot.guild_id,
        };
    } catch (...) {
        return std::unexpected(
            ports::DispatchError{.message = "Unable to snapshot the DonBot configuration"});
    }
    return worker_->enqueue(std::move(request));
}

std::optional<ports::UploadResult> DonBotProviderWorker::try_take_result() {
    return worker_->try_take_result();
}

void DonBotProviderWorker::cancel_pending() noexcept {
    if (worker_) {
        worker_->cancel_pending();
    }
}

std::expected<void, DonBotProviderWorkerError>
DonBotProviderWorker::update_config(DonBotProviderConfig config) {
    if (!valid_config(config)) {
        return std::unexpected(
            make_worker_error(DonBotProviderWorkerErrorCode::InvalidConfiguration,
                              "DonBot provider configuration is invalid"));
    }
    try {
        const std::scoped_lock lock{config_mutex_};
        config_ = std::move(config);
        return {};
    } catch (...) {
        return std::unexpected(
            make_worker_error(DonBotProviderWorkerErrorCode::InvalidConfiguration,
                              "DonBot provider configuration could not be updated"));
    }
}

DonBotProviderConfig DonBotProviderWorker::config_snapshot() const {
    const std::scoped_lock lock{config_mutex_};
    return config_;
}

std::optional<ports::UploadResult>
DonBotProviderWorker::wait_for_result(std::chrono::milliseconds timeout) {
    return worker_->wait_for_result(timeout);
}

std::size_t DonBotProviderWorker::pending_count() const noexcept {
    return worker_->pending_count();
}

std::size_t DonBotProviderWorker::result_count() const noexcept {
    return worker_->result_count();
}

bool DonBotProviderWorker::is_stopping() const noexcept {
    return worker_->is_stopping();
}

std::expected<void, AsyncUploadWorkerError>
DonBotProviderWorker::update_parallelism(std::size_t parallelism) {
    return worker_->update_parallelism(parallelism);
}

std::size_t DonBotProviderWorker::parallelism() const noexcept {
    return worker_->parallelism();
}

ports::UploadResult DonBotProviderWorker::process(const ports::UploadRequest& request,
                                                  const std::stop_token& stop_token) const {
    if (!request.donbot_context) {
        return make_result(request.job_id, ports::UploadOutcome::Failed,
                           "The DonBot upload configuration is unavailable");
    }
    auto api_key = secret_store_.load(ports::SecretId::DonBotGw2ApiKey);
    if (!api_key) {
        return make_result(request.job_id, ports::UploadOutcome::Failed,
                           api_key.error().code == ports::SecretStoreErrorCode::NotFound
                               ? "The DonBot GW2 API key is not configured"
                               : "The DonBot GW2 API key could not be loaded");
    }

    auto uploaded = client_.upload(request.file, request.donbot_context->api_base_url,
                                   request.donbot_context->guild_id, *api_key, stop_token);
    if (!uploaded) {
        switch (uploaded.error().disposition) {
        case DonBotDisposition::Retry:
            return make_result(request.job_id, ports::UploadOutcome::Retry,
                               std::move(uploaded.error().detail),
                               bounded_retry_delay(uploaded.error().retry_after));
        case DonBotDisposition::Failed:
            return make_result(request.job_id, ports::UploadOutcome::Failed,
                               std::move(uploaded.error().detail));
        case DonBotDisposition::Cancelled:
            return make_result(request.job_id, ports::UploadOutcome::Cancelled,
                               std::move(uploaded.error().detail));
        }
    }

    auto detail = std::string{"Uploaded to DonBot"};
    if (uploaded->fight_log_id) {
        detail = "Uploaded and processed by DonBot (fight " +
                 std::to_string(*uploaded->fight_log_id) + ")";
    } else if (uploaded->upload_id) {
        detail = "Uploaded to DonBot (upload " + std::to_string(*uploaded->upload_id) + ")";
    }

    return make_result(request.job_id, ports::UploadOutcome::Succeeded, std::move(detail),
                       std::nullopt,
                       domain::DonBotUploadReceipt{
                           .upload_id = uploaded->upload_id,
                           .fight_log_id = uploaded->fight_log_id,
                       });
}

} // namespace manny_uploader::providers
