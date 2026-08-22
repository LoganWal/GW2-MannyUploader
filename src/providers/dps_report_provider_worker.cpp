#include "manny_uploader/providers/dps_report_provider_worker.hpp"

#include <utility>

namespace manny_uploader::providers {
namespace {

constexpr auto default_retry_delay = std::chrono::seconds{30};

[[nodiscard]] DpsReportProviderWorkerError make_worker_error(DpsReportProviderWorkerErrorCode code,
                                                             std::string message) {
    return DpsReportProviderWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::UploadResult
make_result(domain::UploadJobId job_id, ports::UploadOutcome outcome, std::string detail,
            std::optional<std::chrono::seconds> retry_after = std::nullopt,
            std::optional<domain::DpsReportResult> report = std::nullopt) {
    return ports::UploadResult{
        .job_id = job_id,
        .provider = domain::Provider::DpsReport,
        .outcome = outcome,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .dps_report_result = std::move(report),
        .twitch_delivery_receipt = std::nullopt,
    };
}

[[nodiscard]] std::chrono::seconds
bounded_retry_delay(std::optional<std::chrono::seconds> delay) noexcept {
    if (!delay.has_value() || *delay <= std::chrono::seconds::zero() ||
        *delay > std::chrono::hours{24}) {
        return default_retry_delay;
    }
    return *delay;
}

[[nodiscard]] std::string success_detail(const DpsReportUploadSuccess& success) {
    if (success.warning.has_value()) {
        return *success.warning;
    }
    return "Uploaded to dps.report";
}

} // namespace

std::expected<std::unique_ptr<DpsReportProviderWorker>, DpsReportProviderWorkerError>
DpsReportProviderWorker::create(const IDpsReportClient& client, ports::ISecretStore* secret_store,
                                std::size_t queue_capacity, std::size_t parallelism) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(DpsReportProviderWorkerErrorCode::InvalidCapacity,
                              "dps.report provider queue capacity must be non-zero"));
    }

    try {
        auto provider = std::unique_ptr<DpsReportProviderWorker>{
            new DpsReportProviderWorker{client, secret_store}};
        auto worker = AsyncUploadWorker::create(domain::Provider::DpsReport, *provider,
                                                "The dps.report worker failed unexpectedly",
                                                queue_capacity, parallelism);
        if (!worker) {
            return std::unexpected(
                make_worker_error(DpsReportProviderWorkerErrorCode::ThreadStartFailed,
                                  "Unable to start the dps.report provider worker"));
        }
        provider->worker_ = std::move(*worker);
        return provider;
    } catch (...) {
        return std::unexpected(
            make_worker_error(DpsReportProviderWorkerErrorCode::ThreadStartFailed,
                              "Unable to start the dps.report provider worker"));
    }
}

DpsReportProviderWorker::DpsReportProviderWorker(const IDpsReportClient& client,
                                                 ports::ISecretStore* secret_store)
    : client_{client}, secret_store_{secret_store} {}

DpsReportProviderWorker::~DpsReportProviderWorker() {
    cancel_pending();
}

domain::Provider DpsReportProviderWorker::provider() const noexcept {
    return domain::Provider::DpsReport;
}

std::expected<void, ports::DispatchError>
DpsReportProviderWorker::enqueue(ports::UploadRequest request) {
    if (request.dps_report_result.has_value() || request.donbot_context.has_value() ||
        request.twitch_context.has_value()) {
        return std::unexpected(
            ports::DispatchError{.message = "dps.report upload request is invalid"});
    }
    return worker_->enqueue(std::move(request));
}

std::optional<ports::UploadResult> DpsReportProviderWorker::try_take_result() {
    return worker_->try_take_result();
}

void DpsReportProviderWorker::cancel_pending() noexcept {
    if (worker_) {
        worker_->cancel_pending();
    }
}

std::optional<ports::UploadResult>
DpsReportProviderWorker::wait_for_result(std::chrono::milliseconds timeout) {
    return worker_->wait_for_result(timeout);
}

std::size_t DpsReportProviderWorker::pending_count() const noexcept {
    return worker_->pending_count();
}

std::size_t DpsReportProviderWorker::result_count() const noexcept {
    return worker_->result_count();
}

bool DpsReportProviderWorker::is_stopping() const noexcept {
    return worker_->is_stopping();
}

std::expected<void, AsyncUploadWorkerError>
DpsReportProviderWorker::update_parallelism(std::size_t parallelism) {
    return worker_->update_parallelism(parallelism);
}

std::size_t DpsReportProviderWorker::parallelism() const noexcept {
    return worker_->parallelism();
}

ports::UploadResult DpsReportProviderWorker::process(const ports::UploadRequest& request,
                                                     const std::stop_token& stop_token) const {
    std::optional<support::SecretValue> current_token;
    if (secret_store_ != nullptr) {
        auto loaded = secret_store_->load(ports::SecretId::DpsReportUserToken);
        if (loaded) {
            current_token = std::move(*loaded);
        } else if (loaded.error().code != ports::SecretStoreErrorCode::NotFound) {
            return make_result(request.job_id, ports::UploadOutcome::Failed,
                               "The dps.report user token could not be loaded");
        }
    }

    auto uploaded =
        client_.upload(request.file, current_token ? &*current_token : nullptr, stop_token);
    if (!uploaded) {
        switch (uploaded.error().disposition) {
        case DpsReportUploadDisposition::Retry:
            return make_result(request.job_id, ports::UploadOutcome::Retry,
                               std::move(uploaded.error().detail),
                               bounded_retry_delay(uploaded.error().retry_after));
        case DpsReportUploadDisposition::Failed:
            return make_result(request.job_id, ports::UploadOutcome::Failed,
                               std::move(uploaded.error().detail));
        case DpsReportUploadDisposition::Cancelled:
            return make_result(request.job_id, ports::UploadOutcome::Cancelled,
                               std::move(uploaded.error().detail));
        }
    }

    auto detail = success_detail(*uploaded);
    if (uploaded->replacement_user_token.has_value()) {
        if (secret_store_ == nullptr) {
            detail = "Uploaded to dps.report; protected token storage is unavailable";
        } else {
            const auto stored = secret_store_->store(ports::SecretId::DpsReportUserToken,
                                                     *uploaded->replacement_user_token);
            if (!stored) {
                return make_result(
                    request.job_id, ports::UploadOutcome::Failed,
                    "dps.report uploaded the log, but the returned user token could not be saved");
            }
        }
    }
    return make_result(request.job_id, ports::UploadOutcome::Succeeded, std::move(detail),
                       std::nullopt, std::move(uploaded->report));
}

} // namespace manny_uploader::providers
