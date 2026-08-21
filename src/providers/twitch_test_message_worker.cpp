#include "manny_uploader/providers/twitch_test_message_worker.hpp"

#include <exception>
#include <utility>

namespace manny_uploader::providers {
namespace {

[[nodiscard]] TwitchTestMessageWorkerError make_worker_error(TwitchTestMessageWorkerErrorCode code,
                                                             std::string message) {
    return TwitchTestMessageWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::TwitchTestMessageOutcome
map_outcome(TwitchChatDeliveryOutcome outcome) noexcept {
    switch (outcome) {
    case TwitchChatDeliveryOutcome::Sent:
        return ports::TwitchTestMessageOutcome::Sent;
    case TwitchChatDeliveryOutcome::Dropped:
        return ports::TwitchTestMessageOutcome::Dropped;
    case TwitchChatDeliveryOutcome::Retry:
        return ports::TwitchTestMessageOutcome::Retry;
    case TwitchChatDeliveryOutcome::Failed:
        return ports::TwitchTestMessageOutcome::Failed;
    case TwitchChatDeliveryOutcome::Cancelled:
        return ports::TwitchTestMessageOutcome::Cancelled;
    }
    return ports::TwitchTestMessageOutcome::Failed;
}

[[nodiscard]] ports::TwitchTestMessageResult unexpected_result(std::uint64_t request_id) {
    return ports::TwitchTestMessageResult{
        .request_id = request_id,
        .outcome = ports::TwitchTestMessageOutcome::Failed,
        .detail = "The Twitch test message failed unexpectedly",
        .retry_after = std::nullopt,
        .delivery_status = std::nullopt,
        .delivery_ambiguous = false,
    };
}

} // namespace

std::string make_twitch_test_message(std::uint64_t request_id) {
    return std::string{twitch_test_message_prefix} + std::to_string(request_id);
}

std::expected<std::unique_ptr<TwitchTestMessageWorker>, TwitchTestMessageWorkerError>
TwitchTestMessageWorker::create(const ITwitchClient& client,
                                const ports::ITwitchDeliverySessionAccess& session_access,
                                std::size_t queue_capacity) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(TwitchTestMessageWorkerErrorCode::InvalidCapacity,
                              "Twitch test-message queue capacity must be non-zero"));
    }
    try {
        auto worker = std::unique_ptr<TwitchTestMessageWorker>{
            new TwitchTestMessageWorker{client, session_access, queue_capacity}};
        worker->thread_ = std::jthread{
            [instance = worker.get()](std::stop_token token) { instance->run(std::move(token)); }};
        return worker;
    } catch (...) {
        return std::unexpected(
            make_worker_error(TwitchTestMessageWorkerErrorCode::ThreadStartFailed,
                              "Unable to start Twitch test-message delivery"));
    }
}

TwitchTestMessageWorker::TwitchTestMessageWorker(
    const ITwitchClient& client, const ports::ITwitchDeliverySessionAccess& session_access,
    std::size_t queue_capacity) noexcept
    : delivery_{client, session_access}, queue_capacity_{queue_capacity} {}

TwitchTestMessageWorker::~TwitchTestMessageWorker() {
    cancel_pending();
}

std::expected<void, ports::TwitchTestMessageDispatchError>
TwitchTestMessageWorker::enqueue(ports::TwitchTestMessageRequest request) {
    std::unique_lock lock{mutex_};
    if (stopping_) {
        return std::unexpected(ports::TwitchTestMessageDispatchError{
            .message = "Twitch test-message delivery is stopping"});
    }
    if (request.request_id == 0) {
        return std::unexpected(ports::TwitchTestMessageDispatchError{
            .message = "Twitch test-message request ID must be non-zero"});
    }
    if (requests_.size() >= queue_capacity_) {
        return std::unexpected(
            ports::TwitchTestMessageDispatchError{.message = "Twitch test-message queue is full"});
    }
    try {
        requests_.push_back(request);
    } catch (...) {
        return std::unexpected(ports::TwitchTestMessageDispatchError{
            .message = "Unable to queue the Twitch test message"});
    }
    lock.unlock();
    condition_.notify_all();
    return {};
}

std::optional<ports::TwitchTestMessageResult> TwitchTestMessageWorker::try_take_result() {
    std::unique_lock lock{mutex_};
    auto result = take_result_locked();
    lock.unlock();
    if (result) {
        condition_.notify_all();
    }
    return result;
}

void TwitchTestMessageWorker::cancel_pending() noexcept {
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

std::optional<ports::TwitchTestMessageResult>
TwitchTestMessageWorker::wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    condition_.wait_for(lock, timeout, [this] { return stopping_ || !results_.empty(); });
    auto result = take_result_locked();
    lock.unlock();
    if (result) {
        condition_.notify_all();
    }
    return result;
}

std::size_t TwitchTestMessageWorker::pending_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return requests_.size();
}

std::size_t TwitchTestMessageWorker::result_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return results_.size();
}

bool TwitchTestMessageWorker::is_stopping() const noexcept {
    const std::scoped_lock lock{mutex_};
    return stopping_;
}

void TwitchTestMessageWorker::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        ports::TwitchTestMessageRequest request{};
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this, &stop_token] {
                return stop_token.stop_requested() || !requests_.empty();
            });
            if (stop_token.stop_requested()) {
                return;
            }
            request = requests_.front();
            requests_.pop_front();
        }
        condition_.notify_all();

        auto result = execute(request, stop_token);
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this, &stop_token] {
            return stop_token.stop_requested() || results_.size() < queue_capacity_;
        });
        if (stop_token.stop_requested()) {
            return;
        }
        try {
            results_.push_back(std::move(result));
        } catch (...) {
            stopping_ = true;
            requests_.clear();
            results_.clear();
            thread_.request_stop();
            lock.unlock();
            condition_.notify_all();
            return;
        }
        lock.unlock();
        condition_.notify_all();
    }
}

ports::TwitchTestMessageResult
TwitchTestMessageWorker::execute(ports::TwitchTestMessageRequest request,
                                 const std::stop_token& stop_token) const {
    try {
        auto delivery = delivery_.send(make_twitch_test_message(request.request_id), stop_token);
        const auto delivery_status =
            delivery.receipt ? std::optional{delivery.receipt->status} : std::nullopt;
        const auto outcome = map_outcome(delivery.outcome);
        auto detail = outcome == ports::TwitchTestMessageOutcome::Sent
                          ? std::string{"Twitch test message sent"}
                          : std::move(delivery.detail);
        return ports::TwitchTestMessageResult{
            .request_id = request.request_id,
            .outcome = outcome,
            .detail = std::move(detail),
            .retry_after = delivery.retry_after,
            .delivery_status = delivery_status,
            .delivery_ambiguous = delivery.delivery_ambiguous,
        };
    } catch (...) {
        return unexpected_result(request.request_id);
    }
}

std::optional<ports::TwitchTestMessageResult> TwitchTestMessageWorker::take_result_locked() {
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

} // namespace manny_uploader::providers
