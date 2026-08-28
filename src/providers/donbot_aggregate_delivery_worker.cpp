#include "manny_uploader/providers/donbot_aggregate_delivery_worker.hpp"

#include <utility>

namespace manny_uploader::providers {
namespace {

[[nodiscard]] ports::DonBotAggregateDeliveryResult failed_result(
    std::uint64_t request_id, std::size_t fight_count, std::string detail,
    ports::DonBotAggregateDeliveryOutcome outcome = ports::DonBotAggregateDeliveryOutcome::Failed) {
    return ports::DonBotAggregateDeliveryResult{
        .request_id = request_id,
        .outcome = outcome,
        .fight_log_count = fight_count,
        .detail = std::move(detail),
        .discord_delivery = std::nullopt,
    };
}

[[nodiscard]] ports::DonBotAggregateDeliveryOutcome
aggregate_outcome(const domain::DonBotDiscordDeliveryReceipt& receipt) noexcept {
    if (receipt.outcome == domain::DonBotDiscordDeliveryOutcome::Ambiguous ||
        receipt.ambiguous != 0) {
        return ports::DonBotAggregateDeliveryOutcome::Ambiguous;
    }
    if (receipt.outcome == domain::DonBotDiscordDeliveryOutcome::Failed ||
        receipt.outcome == domain::DonBotDiscordDeliveryOutcome::Skipped ||
        receipt.outcome == domain::DonBotDiscordDeliveryOutcome::NotRequested ||
        (receipt.outcome == domain::DonBotDiscordDeliveryOutcome::Partial && receipt.sent == 0)) {
        return ports::DonBotAggregateDeliveryOutcome::Failed;
    }
    return ports::DonBotAggregateDeliveryOutcome::Succeeded;
}

[[nodiscard]] std::string aggregate_detail(const domain::DonBotDiscordDeliveryReceipt& receipt) {
    std::string outcome;
    switch (receipt.outcome) {
    case domain::DonBotDiscordDeliveryOutcome::NotRequested:
        outcome = "returned an invalid result";
        break;
    case domain::DonBotDiscordDeliveryOutcome::Sent:
        outcome = "was sent";
        break;
    case domain::DonBotDiscordDeliveryOutcome::Partial:
        outcome = "partially completed";
        break;
    case domain::DonBotDiscordDeliveryOutcome::Skipped:
        outcome = "was skipped";
        break;
    case domain::DonBotDiscordDeliveryOutcome::Failed:
        outcome = "failed";
        break;
    case domain::DonBotDiscordDeliveryOutcome::Ambiguous:
        outcome = "could not be confirmed";
        break;
    }
    return "DonBot aggregate delivery " + outcome + ": " + std::to_string(receipt.sent) +
           " sent, " + std::to_string(receipt.skipped) + " skipped, " +
           std::to_string(receipt.failed) + " failed, " + std::to_string(receipt.ambiguous) +
           " ambiguous";
}

} // namespace

std::expected<std::unique_ptr<DonBotAggregateDeliveryWorker>, DonBotAggregateDeliveryWorkerError>
DonBotAggregateDeliveryWorker::create(const IDonBotClient& client,
                                      const ports::ISecretStore& secret_store) {
    try {
        auto worker = std::unique_ptr<DonBotAggregateDeliveryWorker>{
            new DonBotAggregateDeliveryWorker{client, secret_store}};
        worker->thread_ = std::jthread{
            [instance = worker.get()](std::stop_token token) { instance->run(std::move(token)); }};
        return worker;
    } catch (...) {
        return std::unexpected(DonBotAggregateDeliveryWorkerError{
            .message = "Unable to start DonBot aggregate delivery"});
    }
}

DonBotAggregateDeliveryWorker::DonBotAggregateDeliveryWorker(
    const IDonBotClient& client, const ports::ISecretStore& secret_store) noexcept
    : client_{client}, secret_store_{secret_store} {}

DonBotAggregateDeliveryWorker::~DonBotAggregateDeliveryWorker() {
    cancel_pending();
}

std::expected<void, ports::DonBotAggregateDeliveryDispatchError>
DonBotAggregateDeliveryWorker::enqueue(ports::DonBotAggregateDeliveryRequest request) {
    std::unique_lock lock{mutex_};
    if (stopping_) {
        return std::unexpected(ports::DonBotAggregateDeliveryDispatchError{
            .message = "DonBot aggregate delivery is stopping"});
    }
    if (request.request_id == 0 || request.fight_log_ids.size() < 2 ||
        request.fight_log_ids.size() > ports::max_donbot_aggregate_fight_logs || active_ ||
        !requests_.empty() || !results_.empty()) {
        return std::unexpected(ports::DonBotAggregateDeliveryDispatchError{
            .message = active_ || !requests_.empty() || !results_.empty()
                           ? "A DonBot aggregate delivery is active"
                           : "The DonBot aggregate request is invalid"});
    }
    try {
        requests_.push_back(std::move(request));
    } catch (...) {
        return std::unexpected(ports::DonBotAggregateDeliveryDispatchError{
            .message = "Unable to queue DonBot aggregate delivery"});
    }
    lock.unlock();
    condition_.notify_all();
    return {};
}

std::optional<ports::DonBotAggregateDeliveryResult>
DonBotAggregateDeliveryWorker::try_take_result() {
    const std::scoped_lock lock{mutex_};
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

bool DonBotAggregateDeliveryWorker::busy() const noexcept {
    const std::scoped_lock lock{mutex_};
    return active_ || !requests_.empty();
}

void DonBotAggregateDeliveryWorker::cancel_pending() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_) {
            return;
        }
        stopping_ = true;
        requests_.clear();
        results_.clear();
        thread_.request_stop();
    }
    condition_.notify_all();
}

void DonBotAggregateDeliveryWorker::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        ports::DonBotAggregateDeliveryRequest request;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this, &stop_token] {
                return stop_token.stop_requested() || !requests_.empty();
            });
            if (stop_token.stop_requested()) {
                return;
            }
            request = std::move(requests_.front());
            requests_.pop_front();
            active_ = true;
        }

        auto result = execute(std::move(request), stop_token);
        {
            const std::scoped_lock lock{mutex_};
            active_ = false;
            if (!stopping_) {
                try {
                    results_.push_back(std::move(result));
                } catch (...) {
                    stopping_ = true;
                    thread_.request_stop();
                }
            }
        }
        condition_.notify_all();
    }
}

ports::DonBotAggregateDeliveryResult
DonBotAggregateDeliveryWorker::execute(ports::DonBotAggregateDeliveryRequest request,
                                       const std::stop_token& stop_token) const {
    try {
        if (stop_token.stop_requested()) {
            return failed_result(request.request_id, request.fight_log_ids.size(),
                                 "The DonBot aggregate request was cancelled",
                                 ports::DonBotAggregateDeliveryOutcome::Cancelled);
        }
        auto api_key = secret_store_.load(ports::SecretId::DonBotGw2ApiKey);
        if (!api_key) {
            return failed_result(request.request_id, request.fight_log_ids.size(),
                                 api_key.error().code == ports::SecretStoreErrorCode::NotFound
                                     ? "The DonBot GW2 API key is not configured"
                                     : "The DonBot GW2 API key could not be loaded");
        }
        if (stop_token.stop_requested()) {
            return failed_result(request.request_id, request.fight_log_ids.size(),
                                 "The DonBot aggregate request was cancelled",
                                 ports::DonBotAggregateDeliveryOutcome::Cancelled);
        }
        auto delivered = client_.deliver_aggregate(request.fight_log_ids, request.api_base_url,
                                                   request.guild_id, *api_key,
                                                   DonBotDiscordDeliveryRequest{
                                                       .mode = request.delivery_mode,
                                                       .channel_id = request.channel_id,
                                                   },
                                                   stop_token);
        if (!delivered) {
            if (delivered.error().disposition == DonBotDisposition::Cancelled) {
                return failed_result(request.request_id, request.fight_log_ids.size(),
                                     "DonBot aggregate delivery could not be confirmed",
                                     ports::DonBotAggregateDeliveryOutcome::Ambiguous);
            }
            const auto ambiguous = delivered.error().http_error.has_value() ||
                                   delivered.error().http_status.value_or(0) == 408 ||
                                   delivered.error().http_status.value_or(0) == 429 ||
                                   delivered.error().http_status.value_or(0) >= 500 ||
                                   delivered.error().http_status.value_or(0) == 200;
            return failed_result(request.request_id, request.fight_log_ids.size(),
                                 ambiguous ? "DonBot aggregate delivery could not be confirmed"
                                           : std::move(delivered.error().detail),
                                 ambiguous ? ports::DonBotAggregateDeliveryOutcome::Ambiguous
                                           : ports::DonBotAggregateDeliveryOutcome::Failed);
        }
        const auto outcome = aggregate_outcome(delivered->discord_delivery);
        return ports::DonBotAggregateDeliveryResult{
            .request_id = request.request_id,
            .outcome = outcome,
            .fight_log_count = delivered->fight_log_count,
            .detail = aggregate_detail(delivered->discord_delivery),
            .discord_delivery = delivered->discord_delivery,
        };
    } catch (...) {
        return failed_result(request.request_id, request.fight_log_ids.size(),
                             "DonBot aggregate delivery failed unexpectedly");
    }
}

} // namespace manny_uploader::providers
