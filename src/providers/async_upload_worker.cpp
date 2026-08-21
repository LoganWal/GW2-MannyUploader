#include "manny_uploader/providers/async_upload_worker.hpp"

#include <utility>

namespace manny_uploader::providers {
namespace {

[[nodiscard]] AsyncUploadWorkerError make_error(AsyncUploadWorkerErrorCode code,
                                                std::string message) {
    return AsyncUploadWorkerError{.code = code, .message = std::move(message)};
}

} // namespace

std::expected<std::unique_ptr<AsyncUploadWorker>, AsyncUploadWorkerError>
AsyncUploadWorker::create(domain::Provider provider, const IUploadRequestProcessor& processor,
                          std::string unexpected_failure_detail, std::size_t queue_capacity) {
    if (domain::provider_index(provider) >= domain::provider_count) {
        return std::unexpected(make_error(AsyncUploadWorkerErrorCode::InvalidProvider,
                                          "Upload worker provider is invalid"));
    }
    if (queue_capacity == 0) {
        return std::unexpected(make_error(AsyncUploadWorkerErrorCode::InvalidCapacity,
                                          "Upload worker capacity must be non-zero"));
    }

    try {
        auto worker = std::unique_ptr<AsyncUploadWorker>{new AsyncUploadWorker{
            provider, processor, std::move(unexpected_failure_detail), queue_capacity}};
        worker->thread_ = std::jthread{
            [instance = worker.get()](std::stop_token token) { instance->run(std::move(token)); }};
        return worker;
    } catch (...) {
        return std::unexpected(make_error(AsyncUploadWorkerErrorCode::ThreadStartFailed,
                                          "Unable to start the upload worker"));
    }
}

AsyncUploadWorker::AsyncUploadWorker(domain::Provider provider,
                                     const IUploadRequestProcessor& processor,
                                     std::string unexpected_failure_detail,
                                     std::size_t queue_capacity)
    : provider_{provider}, processor_{processor},
      unexpected_failure_detail_{std::move(unexpected_failure_detail)},
      queue_capacity_{queue_capacity} {}

AsyncUploadWorker::~AsyncUploadWorker() {
    cancel_pending();
}

domain::Provider AsyncUploadWorker::provider() const noexcept {
    return provider_;
}

std::expected<void, ports::DispatchError> AsyncUploadWorker::enqueue(ports::UploadRequest request) {
    if (request.job_id.value == 0 || request.provider != provider_ ||
        request.file.canonical_path.empty() || request.attempt == 0) {
        return std::unexpected(ports::DispatchError{.message = "Upload request is invalid"});
    }

    std::unique_lock lock{mutex_};
    if (stopping_) {
        return std::unexpected(ports::DispatchError{.message = "Upload provider is stopping"});
    }
    if (requests_.size() >= queue_capacity_) {
        return std::unexpected(ports::DispatchError{.message = "Upload provider queue is full"});
    }
    try {
        requests_.push_back(std::move(request));
    } catch (...) {
        return std::unexpected(ports::DispatchError{.message = "Unable to queue the upload"});
    }
    lock.unlock();
    condition_.notify_all();
    return {};
}

std::optional<ports::UploadResult> AsyncUploadWorker::try_take_result() {
    std::unique_lock lock{mutex_};
    auto result = take_result_locked();
    lock.unlock();
    if (result.has_value()) {
        condition_.notify_all();
    }
    return result;
}

void AsyncUploadWorker::cancel_pending() noexcept {
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

std::optional<ports::UploadResult>
AsyncUploadWorker::wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    condition_.wait_for(lock, timeout, [this] { return stopping_ || !results_.empty(); });
    auto result = take_result_locked();
    lock.unlock();
    if (result.has_value()) {
        condition_.notify_all();
    }
    return result;
}

std::size_t AsyncUploadWorker::pending_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return requests_.size();
}

std::size_t AsyncUploadWorker::result_count() const noexcept {
    const std::scoped_lock lock{mutex_};
    return results_.size();
}

bool AsyncUploadWorker::is_stopping() const noexcept {
    const std::scoped_lock lock{mutex_};
    return stopping_;
}

void AsyncUploadWorker::run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        ports::UploadRequest request;
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

        const auto job_id = request.job_id;
        ports::UploadResult result = unexpected_result(job_id);
        try {
            result = processor_.process(request, stop_token);
        } catch (...) {
            result = unexpected_result(job_id);
        }

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

ports::UploadResult AsyncUploadWorker::unexpected_result(domain::UploadJobId job_id) const {
    return ports::UploadResult{
        .job_id = job_id,
        .provider = provider_,
        .outcome = ports::UploadOutcome::Failed,
        .detail = unexpected_failure_detail_,
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
        .twitch_delivery_receipt = std::nullopt,
    };
}

std::optional<ports::UploadResult> AsyncUploadWorker::take_result_locked() {
    if (results_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
}

} // namespace manny_uploader::providers
