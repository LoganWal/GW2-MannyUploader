#pragma once

#include "manny_uploader/ports/secret_store.hpp"
#include "manny_uploader/ports/upload_provider.hpp"
#include "manny_uploader/providers/async_upload_worker.hpp"
#include "manny_uploader/providers/donbot_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace manny_uploader::providers {

struct DonBotProviderConfig {
    std::string api_base_url;
    std::string guild_id;
    domain::DonBotDiscordDeliveryMode discord_delivery_mode{
        domain::DonBotDiscordDeliveryMode::None};
    std::string discord_channel_id;

    [[nodiscard]] friend bool operator==(const DonBotProviderConfig&,
                                         const DonBotProviderConfig&) noexcept = default;
};

enum class DonBotProviderWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    InvalidConfiguration,
    ThreadStartFailed,
};

struct DonBotProviderWorkerError {
    DonBotProviderWorkerErrorCode code;
    std::string message;
};

class DonBotProviderWorker final : public ports::IUploadProvider, private IUploadRequestProcessor {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<DonBotProviderWorker>,
                                       DonBotProviderWorkerError>
    create(const IDonBotClient& client, const ports::ISecretStore& secret_store,
           DonBotProviderConfig config, std::size_t queue_capacity = 8,
           std::size_t parallelism = 1);

    ~DonBotProviderWorker() override;

    DonBotProviderWorker(const DonBotProviderWorker&) = delete;
    DonBotProviderWorker& operator=(const DonBotProviderWorker&) = delete;

    [[nodiscard]] domain::Provider provider() const noexcept override;
    [[nodiscard]] std::expected<void, ports::DispatchError>
    enqueue(ports::UploadRequest request) override;
    [[nodiscard]] std::optional<ports::UploadResult> try_take_result() override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::expected<void, DonBotProviderWorkerError>
    update_config(DonBotProviderConfig config);
    [[nodiscard]] DonBotProviderConfig config_snapshot() const;
    [[nodiscard]] std::optional<ports::UploadResult>
    wait_for_result(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;
    [[nodiscard]] std::expected<void, AsyncUploadWorkerError>
    update_parallelism(std::size_t parallelism);
    [[nodiscard]] std::size_t parallelism() const noexcept;

  private:
    DonBotProviderWorker(const IDonBotClient& client, const ports::ISecretStore& secret_store,
                         DonBotProviderConfig config);

    [[nodiscard]] ports::UploadResult process(const ports::UploadRequest& request,
                                              const std::stop_token& stop_token) const override;

    const IDonBotClient& client_;
    const ports::ISecretStore& secret_store_;
    mutable std::mutex config_mutex_;
    DonBotProviderConfig config_;
    std::unique_ptr<AsyncUploadWorker> worker_;
};

} // namespace manny_uploader::providers
