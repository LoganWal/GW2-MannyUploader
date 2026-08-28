#pragma once

#include "manny_uploader/ports/donbot_aggregate_delivery.hpp"
#include "manny_uploader/ports/secret_store.hpp"
#include "manny_uploader/providers/donbot_client.hpp"

#include <condition_variable>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <thread>

namespace manny_uploader::providers {

struct DonBotAggregateDeliveryWorkerError {
    std::string message;
};

class DonBotAggregateDeliveryWorker final : public ports::IDonBotAggregateDelivery {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<DonBotAggregateDeliveryWorker>,
                                       DonBotAggregateDeliveryWorkerError>
    create(const IDonBotClient& client, const ports::ISecretStore& secret_store);

    ~DonBotAggregateDeliveryWorker() override;

    DonBotAggregateDeliveryWorker(const DonBotAggregateDeliveryWorker&) = delete;
    DonBotAggregateDeliveryWorker& operator=(const DonBotAggregateDeliveryWorker&) = delete;

    [[nodiscard]] std::expected<void, ports::DonBotAggregateDeliveryDispatchError>
    enqueue(ports::DonBotAggregateDeliveryRequest request) override;
    [[nodiscard]] std::optional<ports::DonBotAggregateDeliveryResult> try_take_result() override;
    [[nodiscard]] bool busy() const noexcept override;
    void cancel_pending() noexcept override;

  private:
    DonBotAggregateDeliveryWorker(const IDonBotClient& client,
                                  const ports::ISecretStore& secret_store) noexcept;

    void run(std::stop_token stop_token);
    [[nodiscard]] ports::DonBotAggregateDeliveryResult
    execute(ports::DonBotAggregateDeliveryRequest request, const std::stop_token& stop_token) const;

    const IDonBotClient& client_;
    const ports::ISecretStore& secret_store_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ports::DonBotAggregateDeliveryRequest> requests_;
    std::deque<ports::DonBotAggregateDeliveryResult> results_;
    bool active_{};
    bool stopping_{};
    std::jthread thread_;
};

} // namespace manny_uploader::providers
