#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::ports {

inline constexpr std::size_t max_donbot_aggregate_fight_logs = 100;

struct DonBotAggregateDeliveryRequest {
    std::uint64_t request_id{};
    std::string api_base_url;
    std::string guild_id;
    std::vector<std::uint64_t> fight_log_ids;
    domain::DonBotDiscordDeliveryMode delivery_mode{domain::DonBotDiscordDeliveryMode::None};
    std::string channel_id;
};

struct DonBotAggregateDeliveryDispatchError {
    std::string message;
};

enum class DonBotAggregateDeliveryOutcome : std::uint8_t {
    Succeeded,
    Failed,
    Ambiguous,
    Cancelled,
};

struct DonBotAggregateDeliveryResult {
    std::uint64_t request_id{};
    DonBotAggregateDeliveryOutcome outcome{DonBotAggregateDeliveryOutcome::Failed};
    std::size_t fight_log_count{};
    std::string detail;
    std::optional<domain::DonBotDiscordDeliveryReceipt> discord_delivery;
};

class IDonBotAggregateDelivery {
  public:
    virtual ~IDonBotAggregateDelivery() = default;

    [[nodiscard]] virtual std::expected<void, DonBotAggregateDeliveryDispatchError>
    enqueue(DonBotAggregateDeliveryRequest request) = 0;
    [[nodiscard]] virtual std::optional<DonBotAggregateDeliveryResult> try_take_result() = 0;
    [[nodiscard]] virtual bool busy() const noexcept = 0;
    virtual void cancel_pending() noexcept = 0;
};

} // namespace manny_uploader::ports
