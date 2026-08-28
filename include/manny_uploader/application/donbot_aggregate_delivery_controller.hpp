#pragma once

#include "manny_uploader/application/configuration_service.hpp"
#include "manny_uploader/application/donbot_configuration_controller.hpp"
#include "manny_uploader/application/upload_coordinator.hpp"
#include "manny_uploader/ports/donbot_aggregate_delivery.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::application {

struct SendDonBotAggregateCommand {
    std::vector<domain::UploadJobId> job_ids;
    std::uint64_t configuration_revision{};
    std::uint64_t donbot_revision{};
};

enum class DonBotAggregateDeliveryState : std::uint8_t {
    Idle,
    Queued,
    Sending,
    Succeeded,
    Failed,
    Ambiguous,
    Cancelled,
    ShuttingDown,
};

struct DonBotAggregateDeliverySnapshot {
    DonBotAggregateDeliveryState state{DonBotAggregateDeliveryState::Idle};
    std::size_t fight_log_count{};
    std::string detail;
    std::optional<domain::DonBotDiscordDeliveryReceipt> discord_delivery;
    std::uint64_t revision{1};
    bool shutting_down{};
};

enum class DonBotAggregateDeliveryErrorCode : std::uint8_t {
    InvalidCommand,
    QueueFull,
    Busy,
    StaleConfiguration,
    Unavailable,
    UnknownJob,
    IneligibleJob,
    DispatchFailed,
    ShuttingDown,
};

struct DonBotAggregateDeliveryError {
    DonBotAggregateDeliveryErrorCode code;
    std::string message;
};

class DonBotAggregateDeliveryController {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<DonBotAggregateDeliveryController>,
                                       DonBotAggregateDeliveryError>
    create(UploadCoordinator& uploads, ConfigurationService& configuration,
           ports::IDonBotAggregateDelivery& delivery);

    [[nodiscard]] std::expected<void, DonBotAggregateDeliveryError>
    submit(SendDonBotAggregateCommand command);
    [[nodiscard]] std::expected<void, DonBotAggregateDeliveryError>
    tick(const DonBotConfigurationSnapshot& donbot);
    [[nodiscard]] DonBotAggregateDeliverySnapshot snapshot() const;
    void shutdown() noexcept;

  private:
    DonBotAggregateDeliveryController(UploadCoordinator& uploads,
                                      ConfigurationService& configuration,
                                      ports::IDonBotAggregateDelivery& delivery) noexcept;

    [[nodiscard]] std::optional<DonBotAggregateDeliveryError>
    dispatch(const SendDonBotAggregateCommand& command, const DonBotConfigurationSnapshot& donbot);
    void publish_result(ports::DonBotAggregateDeliveryResult result);

    UploadCoordinator& uploads_;
    ConfigurationService& configuration_;
    ports::IDonBotAggregateDelivery& delivery_;
    mutable std::mutex mutex_;
    std::deque<SendDonBotAggregateCommand> commands_;
    DonBotAggregateDeliverySnapshot published_;
    std::optional<std::uint64_t> active_request_id_;
    std::uint64_t next_request_id_{1};
    bool ticking_{};
};

} // namespace manny_uploader::application
