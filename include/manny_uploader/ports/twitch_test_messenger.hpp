#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace manny_uploader::ports {

struct TwitchTestMessageRequest {
    std::uint64_t request_id;
};

struct TwitchTestMessageDispatchError {
    std::string message;
};

enum class TwitchTestMessageOutcome : std::uint8_t {
    Sent,
    Dropped,
    Retry,
    Failed,
    Cancelled,
};

struct TwitchTestMessageResult {
    std::uint64_t request_id;
    TwitchTestMessageOutcome outcome;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<domain::TwitchDeliveryStatus> delivery_status;
    bool delivery_ambiguous{};
};

class ITwitchTestMessenger {
  public:
    virtual ~ITwitchTestMessenger() = default;

    [[nodiscard]] virtual std::expected<void, TwitchTestMessageDispatchError>
    enqueue(TwitchTestMessageRequest request) = 0;
    [[nodiscard]] virtual std::optional<TwitchTestMessageResult> try_take_result() = 0;
    virtual void cancel_pending() noexcept = 0;
};

} // namespace manny_uploader::ports
