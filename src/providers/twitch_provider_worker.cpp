#include "manny_uploader/providers/twitch_provider_worker.hpp"

#include <algorithm>
#include <utility>

namespace manny_uploader::providers {
namespace {

constexpr std::size_t maximum_delivery_ledger_entries = 256;

[[nodiscard]] TwitchProviderWorkerError make_worker_error(TwitchProviderWorkerErrorCode code,
                                                          std::string message) {
    return TwitchProviderWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::UploadResult
make_result(domain::UploadJobId job_id, ports::UploadOutcome outcome, std::string detail,
            std::optional<std::chrono::seconds> retry_after = std::nullopt,
            std::optional<domain::TwitchDeliveryReceipt> receipt = std::nullopt) {
    return ports::UploadResult{
        .job_id = job_id,
        .provider = domain::Provider::Twitch,
        .outcome = outcome,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .dps_report_result = std::nullopt,
        .twitch_delivery_receipt = std::move(receipt),
    };
}

[[nodiscard]] std::optional<TwitchProviderWorkerError>
validate_config(const TwitchProviderConfig& config) {
    if (!config.post_success && !config.post_failure) {
        return make_worker_error(TwitchProviderWorkerErrorCode::InvalidConfiguration,
                                 "Twitch posting requires at least one result policy");
    }
    if (!application::TwitchMessageTemplate::parse(config.message_template)) {
        return make_worker_error(TwitchProviderWorkerErrorCode::InvalidConfiguration,
                                 "Twitch message template is invalid");
    }
    return std::nullopt;
}

} // namespace

std::expected<std::unique_ptr<TwitchProviderWorker>, TwitchProviderWorkerError>
TwitchProviderWorker::create(const ITwitchClient& client,
                             const ports::ITwitchDeliverySessionAccess& session_access,
                             TwitchProviderConfig config, std::size_t queue_capacity,
                             std::size_t parallelism) {
    if (queue_capacity == 0) {
        return std::unexpected(make_worker_error(TwitchProviderWorkerErrorCode::InvalidCapacity,
                                                 "Twitch provider capacity must be non-zero"));
    }
    if (const auto invalid = validate_config(config)) {
        return std::unexpected(*invalid);
    }
    try {
        auto provider = std::unique_ptr<TwitchProviderWorker>{
            new TwitchProviderWorker{client, session_access, std::move(config)}};
        auto worker = AsyncUploadWorker::create(domain::Provider::Twitch, *provider,
                                                "The Twitch worker failed unexpectedly",
                                                queue_capacity, parallelism);
        if (!worker) {
            return std::unexpected(
                make_worker_error(TwitchProviderWorkerErrorCode::ThreadStartFailed,
                                  "Unable to start the Twitch provider worker"));
        }
        provider->worker_ = std::move(*worker);
        return provider;
    } catch (...) {
        return std::unexpected(make_worker_error(TwitchProviderWorkerErrorCode::ThreadStartFailed,
                                                 "Unable to start the Twitch provider worker"));
    }
}

TwitchProviderWorker::TwitchProviderWorker(
    const ITwitchClient& client, const ports::ITwitchDeliverySessionAccess& session_access,
    TwitchProviderConfig config)
    : delivery_{client, session_access}, config_{std::move(config)} {}

TwitchProviderWorker::~TwitchProviderWorker() {
    cancel_pending();
}

domain::Provider TwitchProviderWorker::provider() const noexcept {
    return domain::Provider::Twitch;
}

std::expected<void, ports::DispatchError>
TwitchProviderWorker::enqueue(ports::UploadRequest request) {
    if (!request.dps_report_result || request.donbot_context || request.twitch_context) {
        return std::unexpected(ports::DispatchError{.message = "Twitch upload request is invalid"});
    }
    try {
        const auto snapshot = config_snapshot();
        request.twitch_context = ports::TwitchUploadContext{
            .message_template = snapshot.message_template,
            .post_success = snapshot.post_success,
            .post_failure = snapshot.post_failure,
        };
    } catch (...) {
        return std::unexpected(
            ports::DispatchError{.message = "Unable to snapshot the Twitch configuration"});
    }
    return worker_->enqueue(std::move(request));
}

std::optional<ports::UploadResult> TwitchProviderWorker::try_take_result() {
    return worker_->try_take_result();
}

void TwitchProviderWorker::cancel_pending() noexcept {
    if (worker_) {
        worker_->cancel_pending();
    }
}

std::expected<void, TwitchProviderWorkerError>
TwitchProviderWorker::update_config(TwitchProviderConfig config) {
    if (const auto invalid = validate_config(config)) {
        return std::unexpected(*invalid);
    }
    try {
        const std::scoped_lock lock{config_mutex_};
        config_ = std::move(config);
        return {};
    } catch (...) {
        return std::unexpected(
            make_worker_error(TwitchProviderWorkerErrorCode::InvalidConfiguration,
                              "Twitch configuration could not be updated"));
    }
}

TwitchProviderConfig TwitchProviderWorker::config_snapshot() const {
    const std::scoped_lock lock{config_mutex_};
    return config_;
}

std::optional<ports::UploadResult>
TwitchProviderWorker::wait_for_result(std::chrono::milliseconds timeout) {
    return worker_->wait_for_result(timeout);
}

std::size_t TwitchProviderWorker::pending_count() const noexcept {
    return worker_->pending_count();
}

std::size_t TwitchProviderWorker::result_count() const noexcept {
    return worker_->result_count();
}

bool TwitchProviderWorker::is_stopping() const noexcept {
    return worker_->is_stopping();
}

std::expected<void, AsyncUploadWorkerError>
TwitchProviderWorker::update_parallelism(std::size_t parallelism) {
    return worker_->update_parallelism(parallelism);
}

std::size_t TwitchProviderWorker::parallelism() const noexcept {
    return worker_->parallelism();
}

ports::UploadResult TwitchProviderWorker::process(const ports::UploadRequest& request,
                                                  const std::stop_token& stop_token) const {
    if (!request.dps_report_result || !request.twitch_context) {
        return make_result(request.job_id, ports::UploadOutcome::Failed,
                           "The Twitch delivery context is unavailable");
    }
    const auto& report = *request.dps_report_result;
    const auto& context = *request.twitch_context;
    if ((report.success && !context.post_success) || (!report.success && !context.post_failure)) {
        return make_result(request.job_id, ports::UploadOutcome::Skipped,
                           "Skipped by the Twitch encounter-result policy");
    }
    const auto parsed = application::TwitchMessageTemplate::parse(context.message_template);
    if (!parsed) {
        return make_result(request.job_id, ports::UploadOutcome::Failed,
                           "The Twitch message template is invalid");
    }
    const auto rendered = parsed->render(report);
    if (!rendered) {
        return make_result(request.job_id, ports::UploadOutcome::Failed, rendered.error().message);
    }
    if (const auto previous = previous_delivery(request)) {
        return *previous;
    }
    return finalize_delivery(request, delivery_.send(*rendered, stop_token));
}

ports::UploadResult
TwitchProviderWorker::finalize_delivery(const ports::UploadRequest& request,
                                        TwitchChatDeliveryResult delivery) const {
    if (delivery.delivery_ambiguous) {
        record_delivery(request, LedgerState::Ambiguous);
    }
    switch (delivery.outcome) {
    case TwitchChatDeliveryOutcome::Sent:
        record_delivery(request, LedgerState::Posted,
                        delivery.receipt ? delivery.receipt->message_id : std::nullopt);
        return make_result(request.job_id, ports::UploadOutcome::Succeeded,
                           std::move(delivery.detail), delivery.retry_after,
                           std::move(delivery.receipt));
    case TwitchChatDeliveryOutcome::Dropped:
    case TwitchChatDeliveryOutcome::Failed:
        return make_result(request.job_id, ports::UploadOutcome::Failed, std::move(delivery.detail),
                           delivery.retry_after, std::move(delivery.receipt));
    case TwitchChatDeliveryOutcome::Retry:
        return make_result(request.job_id, ports::UploadOutcome::Retry, std::move(delivery.detail),
                           delivery.retry_after);
    case TwitchChatDeliveryOutcome::Cancelled:
        return make_result(request.job_id, ports::UploadOutcome::Cancelled,
                           std::move(delivery.detail));
    }
    return make_result(request.job_id, ports::UploadOutcome::Failed,
                       "Twitch delivery failed unexpectedly");
}

std::optional<ports::UploadResult>
TwitchProviderWorker::previous_delivery(const ports::UploadRequest& request) const {
    if (request.user_initiated_retry) {
        return std::nullopt;
    }
    const std::scoped_lock lock{ledger_mutex_};
    const auto found = std::ranges::find_if(ledger_, [&request](const LedgerEntry& entry) {
        return entry.job_id == request.job_id && request.dps_report_result &&
               entry.permalink == request.dps_report_result->permalink;
    });
    if (found == ledger_.end()) {
        return std::nullopt;
    }
    if (found->state == LedgerState::Ambiguous) {
        return make_result(
            request.job_id, ports::UploadOutcome::Failed,
            "Twitch delivery was previously ambiguous; automatic retry was suppressed");
    }
    return make_result(request.job_id, ports::UploadOutcome::Succeeded,
                       "Twitch chat was already posted", std::nullopt,
                       domain::TwitchDeliveryReceipt{
                           .status = domain::TwitchDeliveryStatus::Sent,
                           .message_id = found->message_id,
                       });
}

void TwitchProviderWorker::record_delivery(const ports::UploadRequest& request, LedgerState state,
                                           std::optional<std::string> message_id) const {
    if (!request.dps_report_result) {
        return;
    }
    const std::scoped_lock lock{ledger_mutex_};
    const auto found = std::ranges::find_if(ledger_, [&request](const LedgerEntry& entry) {
        return entry.job_id == request.job_id &&
               entry.permalink == request.dps_report_result->permalink;
    });
    if (found != ledger_.end()) {
        found->state = state;
        found->message_id = std::move(message_id);
        return;
    }
    if (ledger_.size() >= maximum_delivery_ledger_entries) {
        ledger_.pop_front();
    }
    ledger_.push_back(LedgerEntry{
        .job_id = request.job_id,
        .permalink = request.dps_report_result->permalink,
        .state = state,
        .message_id = std::move(message_id),
    });
}

} // namespace manny_uploader::providers
