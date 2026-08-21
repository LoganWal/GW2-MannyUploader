#pragma once

#include "manny_uploader/support/secret_value.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>

namespace manny_uploader::ports {

struct TwitchDeliverySession {
    support::SecretValue access_token;
    std::string authenticated_user_id;
    std::uint64_t revision;
};

enum class TwitchDeliverySessionErrorCode : std::uint8_t {
    NotConnected,
    Retry,
    ReconnectRequired,
    Failed,
    Cancelled,
};

struct TwitchDeliverySessionError {
    TwitchDeliverySessionErrorCode code;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
};

class ITwitchDeliverySessionAccess {
  public:
    virtual ~ITwitchDeliverySessionAccess() = default;

    [[nodiscard]] virtual std::expected<TwitchDeliverySession, TwitchDeliverySessionError>
    acquire(const std::stop_token& stop_token) const = 0;

    [[nodiscard]] virtual std::expected<TwitchDeliverySession, TwitchDeliverySessionError>
    recover(TwitchDeliverySession rejected_session, const std::stop_token& stop_token) const = 0;
};

} // namespace manny_uploader::ports
