#pragma once

#include "manny_uploader/ports/upload_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace manny_uploader::providers {

class IUploadRequestProcessor {
  public:
    virtual ~IUploadRequestProcessor() = default;

    [[nodiscard]] virtual ports::UploadResult process(const ports::UploadRequest& request,
                                                      const std::stop_token& stop_token) const = 0;
};

enum class AsyncUploadWorkerErrorCode : std::uint8_t {
    InvalidProvider,
    InvalidCapacity,
    ThreadStartFailed,
};

struct AsyncUploadWorkerError {
    AsyncUploadWorkerErrorCode code;
    std::string message;
};

class AsyncUploadWorker final : public ports::IUploadProvider {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<AsyncUploadWorker>, AsyncUploadWorkerError>
    create(domain::Provider provider, const IUploadRequestProcessor& processor,
           std::string unexpected_failure_detail, std::size_t queue_capacity = 8,
           std::size_t parallelism = 1);

    ~AsyncUploadWorker() override;

    AsyncUploadWorker(const AsyncUploadWorker&) = delete;
    AsyncUploadWorker& operator=(const AsyncUploadWorker&) = delete;

    [[nodiscard]] domain::Provider provider() const noexcept override;
    [[nodiscard]] std::expected<void, ports::DispatchError>
    enqueue(ports::UploadRequest request) override;
    [[nodiscard]] std::optional<ports::UploadResult> try_take_result() override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::optional<ports::UploadResult>
    wait_for_result(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;
    [[nodiscard]] std::expected<void, AsyncUploadWorkerError>
    update_parallelism(std::size_t parallelism);
    [[nodiscard]] std::size_t parallelism() const noexcept;

  private:
    AsyncUploadWorker(domain::Provider provider, const IUploadRequestProcessor& processor,
                      std::string unexpected_failure_detail, std::size_t queue_capacity);

    [[nodiscard]] std::expected<void, AsyncUploadWorkerError>
    add_threads_until(std::size_t thread_count);

    void run(std::stop_token stop_token);
    [[nodiscard]] ports::UploadResult unexpected_result(domain::UploadJobId job_id) const;
    [[nodiscard]] std::optional<ports::UploadResult> take_result_locked();

    domain::Provider provider_;
    const IUploadRequestProcessor& processor_;
    std::string unexpected_failure_detail_;
    std::size_t queue_capacity_;
    std::size_t parallelism_{1};
    std::size_t active_count_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ports::UploadRequest> requests_;
    std::deque<ports::UploadResult> results_;
    bool stopping_{};
    std::vector<std::jthread> threads_;
};

} // namespace manny_uploader::providers
