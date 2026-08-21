#pragma once

#include "manny_uploader/ports/upload_provider.hpp"
#include "manny_uploader/providers/async_upload_worker.hpp"
#include "manny_uploader/providers/wingman_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace manny_uploader::providers {

enum class WingmanProviderWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    ThreadStartFailed,
};

struct WingmanProviderWorkerError {
    WingmanProviderWorkerErrorCode code;
    std::string message;
};

class WingmanProviderWorker final : public ports::IUploadProvider, private IUploadRequestProcessor {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<WingmanProviderWorker>,
                                       WingmanProviderWorkerError>
    create(const IWingmanClient& client, std::size_t queue_capacity = 8);

    ~WingmanProviderWorker() override;

    WingmanProviderWorker(const WingmanProviderWorker&) = delete;
    WingmanProviderWorker& operator=(const WingmanProviderWorker&) = delete;

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

  private:
    explicit WingmanProviderWorker(const IWingmanClient& client);

    [[nodiscard]] ports::UploadResult process(const ports::UploadRequest& request,
                                              const std::stop_token& stop_token) const override;

    const IWingmanClient& client_;
    std::unique_ptr<AsyncUploadWorker> worker_;
};

} // namespace manny_uploader::providers
