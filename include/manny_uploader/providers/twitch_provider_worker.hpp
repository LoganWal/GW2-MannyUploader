#pragma once

#include "manny_uploader/application/twitch_message_template.hpp"
#include "manny_uploader/ports/twitch_delivery_session.hpp"
#include "manny_uploader/ports/upload_provider.hpp"
#include "manny_uploader/providers/async_upload_worker.hpp"
#include "manny_uploader/providers/twitch_chat_delivery.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace manny_uploader::providers {

struct TwitchProviderConfig {
    std::string message_template;
    bool post_success;
    bool post_failure;

    [[nodiscard]] friend bool operator==(const TwitchProviderConfig&,
                                         const TwitchProviderConfig&) noexcept = default;
};

enum class TwitchProviderWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    InvalidConfiguration,
    ThreadStartFailed,
};

struct TwitchProviderWorkerError {
    TwitchProviderWorkerErrorCode code;
    std::string message;
};

class TwitchProviderWorker final : public ports::IUploadProvider, private IUploadRequestProcessor {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<TwitchProviderWorker>,
                                       TwitchProviderWorkerError>
    create(const ITwitchClient& client, const ports::ITwitchDeliverySessionAccess& session_access,
           TwitchProviderConfig config, std::size_t queue_capacity = 8,
           std::size_t parallelism = 1);

    ~TwitchProviderWorker() override;

    TwitchProviderWorker(const TwitchProviderWorker&) = delete;
    TwitchProviderWorker& operator=(const TwitchProviderWorker&) = delete;

    [[nodiscard]] domain::Provider provider() const noexcept override;
    [[nodiscard]] std::expected<void, ports::DispatchError>
    enqueue(ports::UploadRequest request) override;
    [[nodiscard]] std::optional<ports::UploadResult> try_take_result() override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::expected<void, TwitchProviderWorkerError>
    update_config(TwitchProviderConfig config);
    [[nodiscard]] TwitchProviderConfig config_snapshot() const;
    [[nodiscard]] std::optional<ports::UploadResult>
    wait_for_result(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;
    [[nodiscard]] std::expected<void, AsyncUploadWorkerError>
    update_parallelism(std::size_t parallelism);
    [[nodiscard]] std::size_t parallelism() const noexcept;

  private:
    enum class LedgerState : std::uint8_t {
        Posted,
        Ambiguous,
    };

    struct LedgerEntry {
        domain::UploadJobId job_id;
        std::string permalink;
        LedgerState state;
        std::optional<std::string> message_id;
    };

    TwitchProviderWorker(const ITwitchClient& client,
                         const ports::ITwitchDeliverySessionAccess& session_access,
                         TwitchProviderConfig config);

    [[nodiscard]] ports::UploadResult process(const ports::UploadRequest& request,
                                              const std::stop_token& stop_token) const override;
    [[nodiscard]] ports::UploadResult finalize_delivery(const ports::UploadRequest& request,
                                                        TwitchChatDeliveryResult delivery) const;
    [[nodiscard]] std::optional<ports::UploadResult>
    previous_delivery(const ports::UploadRequest& request) const;
    void record_delivery(const ports::UploadRequest& request, LedgerState state,
                         std::optional<std::string> message_id = std::nullopt) const;

    TwitchChatDelivery delivery_;
    mutable std::mutex config_mutex_;
    mutable std::mutex ledger_mutex_;
    TwitchProviderConfig config_;
    mutable std::deque<LedgerEntry> ledger_;
    std::unique_ptr<AsyncUploadWorker> worker_;
};

} // namespace manny_uploader::providers
