#pragma once

#include "manny_uploader/ports/secret_store.hpp"
#include "manny_uploader/ports/upload_provider.hpp"
#include "manny_uploader/providers/async_upload_worker.hpp"
#include "manny_uploader/providers/dps_report_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace manny_uploader::providers {

enum class DpsReportProviderWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    ThreadStartFailed,
};

struct DpsReportProviderWorkerError {
    DpsReportProviderWorkerErrorCode code;
    std::string message;
};

class DpsReportProviderWorker final : public ports::IUploadProvider,
                                      private IUploadRequestProcessor {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<DpsReportProviderWorker>,
                                       DpsReportProviderWorkerError>
    create(const IDpsReportClient& client, ports::ISecretStore* secret_store = nullptr,
           std::size_t queue_capacity = 8);

    ~DpsReportProviderWorker() override;

    DpsReportProviderWorker(const DpsReportProviderWorker&) = delete;
    DpsReportProviderWorker& operator=(const DpsReportProviderWorker&) = delete;

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
    DpsReportProviderWorker(const IDpsReportClient& client, ports::ISecretStore* secret_store);

    [[nodiscard]] ports::UploadResult process(const ports::UploadRequest& request,
                                              const std::stop_token& stop_token) const override;

    const IDpsReportClient& client_;
    ports::ISecretStore* secret_store_;
    std::unique_ptr<AsyncUploadWorker> worker_;
};

} // namespace manny_uploader::providers
