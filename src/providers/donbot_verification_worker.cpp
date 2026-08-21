#include "manny_uploader/providers/donbot_verification_worker.hpp"

#include <exception>
#include <utility>

namespace manny_uploader::providers {
namespace {

[[nodiscard]] DonBotVerificationWorkerError
make_worker_error(DonBotVerificationWorkerErrorCode code, std::string message) {
    return DonBotVerificationWorkerError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::DonBotVerificationFailure
verification_failure(ports::DonBotVerificationFailureCode code, std::string detail) {
    return ports::DonBotVerificationFailure{.code = code, .detail = std::move(detail)};
}

} // namespace

std::expected<std::unique_ptr<DonBotVerificationWorker>, DonBotVerificationWorkerError>
DonBotVerificationWorker::create(const IDonBotClient& client, std::size_t queue_capacity) {
    if (queue_capacity == 0) {
        return std::unexpected(
            make_worker_error(DonBotVerificationWorkerErrorCode::InvalidCapacity,
                              "DonBot verification queue capacity must be non-zero"));
    }

    try {
        auto worker = std::unique_ptr<DonBotVerificationWorker>{
            new DonBotVerificationWorker{client, queue_capacity}};
        worker->thread_ = std::jthread{
            [instance = worker.get()](std::stop_token token) { instance->run(std::move(token)); }};
        return worker;
    } catch (...) {
        return std::unexpected(
            make_worker_error(DonBotVerificationWorkerErrorCode::ThreadStartFailed,
                              "Unable to start DonBot verification"));
    }
}

DonBotVerificationWorker::DonBotVerificationWorker(const IDonBotClient& client,
                                                   std::size_t queue_capacity)
    : client_{client}, queue_capacity_{queue_capacity} {}

DonBotVerificationWorker::~DonBotVerificationWorker() {
    cancel_pending();
}

std::expected<void, ports::DonBotVerificationDispatchError>
DonBotVerificationWorker::enqueue(ports::DonBotVerificationRequest request) {
    std::unique_lock lock{mutex_};
    if (stopping_) {
        return std::unexpected(
            ports::DonBotVerificationDispatchError{.message = "DonBot verification is stopping"});
    }
    if (requests_.size() >= queue_capacity_) {
        return std::unexpected(
            ports::DonBotVerificationDispatchError{.message = "DonBot verification queue is full"});
    }

    try {
        requests_.push_back(std::move(request));
    } catch (...) {
        return std::unexpected(ports::DonBotVerificationDispatchError{
            .message = "Unable to queue DonBot verification"});
    }
    lock.unlock();
    condition_.notify_all();
    return {};
}

std::optional<ports::DonBotVerificationResult> DonBotVerificationWorker::try_take_result() {
    std::unique_lock lock{mutex_};
    auto result = take_result_locked();
    lock.unlock();
    if (result) {
        condition_.notify_all();
    }
    return result;
}

void DonBotVerificationWorker::cancel_pending() noexcept {
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

std::optional<ports::DonBotVerificationResult>
DonBotVerificationWorker::wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    condition_.wait_for(lock, timeout, [this] { return stopping_ || !results_.empty(); });
    auto result = take_result_locked();
    lock.unlock();
    if (result) {
        condition_.notify_all();
    }
    return result;
}

std::size_t DonBotVerificationWorker::pending_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return requests_.size();
}

std::size_t DonBotVerificationWorker::result_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return results_.size();
}

bool DonBotVerificationWorker::is_stopping() const noexcept {
    const std::scoped_lock lock{mutex_};
    return stopping_;
}

void DonBotVerificationWorker::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        ports::DonBotVerificationRequest request;
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
        }
        condition_.notify_all();

        std::expected<ports::DonBotVerificationSuccess, ports::DonBotVerificationFailure>
            verification =
                std::unexpected(verification_failure(ports::DonBotVerificationFailureCode::Failed,
                                                     "DonBot verification failed unexpectedly"));
        try {
            auto verified = client_.verify(request.api_base_url, request.api_key, stop_token);
            if (verified) {
                verification = ports::DonBotVerificationSuccess{
                    .identity = std::move(*verified),
                    .api_key = std::move(request.api_key),
                };
            } else {
                const auto code = verified.error().disposition == DonBotDisposition::Cancelled
                                      ? ports::DonBotVerificationFailureCode::Cancelled
                                      : ports::DonBotVerificationFailureCode::Failed;
                verification =
                    std::unexpected(verification_failure(code, std::move(verified.error().detail)));
            }
        } catch (...) {
            verification =
                std::unexpected(verification_failure(ports::DonBotVerificationFailureCode::Failed,
                                                     "DonBot verification failed unexpectedly"));
        }

        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this, &stop_token] {
            return stop_token.stop_requested() || results_.size() < queue_capacity_;
        });
        if (stop_token.stop_requested()) {
            return;
        }
        try {
            results_.push_back(ports::DonBotVerificationResult{
                .request_id = request.request_id,
                .verification = std::move(verification),
            });
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

std::optional<ports::DonBotVerificationResult> DonBotVerificationWorker::take_result_locked() {
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

} // namespace manny_uploader::providers
