#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/twitch_delivery_session.hpp"
#include "manny_uploader/providers/twitch_client.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace manny_uploader::providers {

enum class TwitchChatDeliveryOutcome : std::uint8_t {
    Sent,
    Dropped,
    Retry,
    Failed,
    Cancelled,
};

struct TwitchChatDeliveryResult {
    TwitchChatDeliveryOutcome outcome;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<domain::TwitchDeliveryReceipt> receipt;
    bool delivery_ambiguous{};
};

class TwitchChatDelivery {
  public:
    TwitchChatDelivery(const ITwitchClient& client,
                       const ports::ITwitchDeliverySessionAccess& session_access) noexcept;

    [[nodiscard]] TwitchChatDeliveryResult send(std::string_view message,
                                                const std::stop_token& stop_token = {}) const;

  private:
    const ITwitchClient& client_;
    const ports::ITwitchDeliverySessionAccess& session_access_;
};

} // namespace manny_uploader::providers
