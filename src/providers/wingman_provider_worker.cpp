#include "manny_uploader/providers/wingman_provider_worker.hpp"

#include <utility>

namespace manny_uploader::providers {
namespace {

constexpr auto default_retry_delay = std::chrono::seconds{30};

[[nodiscard]] WingmanProviderWorkerError make_worker_error(WingmanProviderWorkerErrorCode code,
                                                           std::string message) {
    return WingmanProviderWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::UploadResult
make_result(domain::UploadJobId job_id, ports::UploadOutcome outcome, std::string detail,
            std::optional<std::chrono::seconds> retry_after = std::nullopt,
            std::optional<domain::WingmanUploadReceipt> receipt = std::nullopt) {
    return ports::UploadResult{
        .job_id = job_id,
        .provider = domain::Provider::Wingman,
        .outcome = outcome,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .dps_report_result = std::nullopt,
        .wingman_upload_receipt = std::move(receipt),
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

} // namespace

std::expected<std::unique_ptr<WingmanProviderWorker>, WingmanProviderWorkerError>
WingmanProviderWorker::create(const IWingmanClient& client, std::size_t queue_capacity,
                              std::size_t parallelism) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(WingmanProviderWorkerErrorCode::InvalidCapacity,
                              "GW2Wingman provider queue capacity must be non-zero"));
    }

    try {
        auto provider = std::unique_ptr<WingmanProviderWorker>{new WingmanProviderWorker{client}};
        auto worker = AsyncUploadWorker::create(domain::Provider::Wingman, *provider,
                                                "The GW2Wingman worker failed unexpectedly",
                                                queue_capacity, parallelism);
        if (!worker) {
            return std::unexpected(
                make_worker_error(WingmanProviderWorkerErrorCode::ThreadStartFailed,
                                  "Unable to start the GW2Wingman provider worker"));
        }
        provider->worker_ = std::move(*worker);
        return provider;
    } catch (...) {
        return std::unexpected(make_worker_error(WingmanProviderWorkerErrorCode::ThreadStartFailed,
                                                 "Unable to start the GW2Wingman provider worker"));
    }
}

WingmanProviderWorker::WingmanProviderWorker(const IWingmanClient& client) : client_{client} {}

WingmanProviderWorker::~WingmanProviderWorker() {
    cancel_pending();
}

domain::Provider WingmanProviderWorker::provider() const noexcept {
    return domain::Provider::Wingman;
}

std::expected<void, ports::DispatchError>
WingmanProviderWorker::enqueue(ports::UploadRequest request) {
    if (request.donbot_context.has_value() || request.twitch_context.has_value()) {
        return std::unexpected(
            ports::DispatchError{.message = "GW2Wingman upload request is invalid"});
    }
    return worker_->enqueue(std::move(request));
}

std::optional<ports::UploadResult> WingmanProviderWorker::try_take_result() {
    return worker_->try_take_result();
}

void WingmanProviderWorker::cancel_pending() noexcept {
    if (worker_) {
        worker_->cancel_pending();
    }
}

std::optional<ports::UploadResult>
WingmanProviderWorker::wait_for_result(std::chrono::milliseconds timeout) {
    return worker_->wait_for_result(timeout);
}

std::size_t WingmanProviderWorker::pending_count() const noexcept {
    return worker_->pending_count();
}

std::size_t WingmanProviderWorker::result_count() const noexcept {
    return worker_->result_count();
}

bool WingmanProviderWorker::is_stopping() const noexcept {
    return worker_->is_stopping();
}

std::expected<void, AsyncUploadWorkerError>
WingmanProviderWorker::update_parallelism(std::size_t parallelism) {
    return worker_->update_parallelism(parallelism);
}

std::size_t WingmanProviderWorker::parallelism() const noexcept {
    return worker_->parallelism();
}

ports::UploadResult WingmanProviderWorker::process(const ports::UploadRequest& request,
                                                   const std::stop_token& stop_token) const {
    auto uploaded = request.dps_report_result
                        ? client_.import_permalink(request.dps_report_result->permalink, stop_token)
                        : client_.upload(request.file, request.metadata, stop_token);
    if (!uploaded) {
        switch (uploaded.error().disposition) {
        case WingmanUploadDisposition::Retry:
            return make_result(request.job_id, ports::UploadOutcome::Retry,
                               std::move(uploaded.error().detail),
                               bounded_retry_delay(uploaded.error().retry_after));
        case WingmanUploadDisposition::Failed:
            return make_result(request.job_id, ports::UploadOutcome::Failed,
                               std::move(uploaded.error().detail));
        case WingmanUploadDisposition::Cancelled:
            return make_result(request.job_id, ports::UploadOutcome::Cancelled,
                               std::move(uploaded.error().detail));
        }
    }

    std::optional<domain::WingmanUploadReceipt> receipt;
    if (uploaded->permalink) {
        receipt = domain::WingmanUploadReceipt{.permalink = std::move(*uploaded->permalink)};
    }
    return make_result(request.job_id, ports::UploadOutcome::Succeeded,
                       uploaded->duplicate         ? "Already present in GW2Wingman"
                       : request.dps_report_result ? "Queued in GW2Wingman"
                                                   : "Uploaded to GW2Wingman",
                       std::nullopt, std::move(receipt));
}

} // namespace manny_uploader::providers
