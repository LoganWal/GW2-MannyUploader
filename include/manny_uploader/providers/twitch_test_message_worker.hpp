#pragma once

#include "manny_uploader/ports/twitch_test_messenger.hpp"
#include "manny_uploader/providers/twitch_chat_delivery.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace manny_uploader::providers {

inline constexpr std::string_view twitch_test_message_prefix = "GW2 Manny Uploader test message #";

[[nodiscard]] std::string make_twitch_test_message(std::uint64_t request_id);

enum class TwitchTestMessageWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    ThreadStartFailed,
};

struct TwitchTestMessageWorkerError {
    TwitchTestMessageWorkerErrorCode code;
    std::string message;
};

class TwitchTestMessageWorker final : public ports::ITwitchTestMessenger {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<TwitchTestMessageWorker>,
                                       TwitchTestMessageWorkerError>
    create(const ITwitchClient& client, const ports::ITwitchDeliverySessionAccess& session_access,
           std::size_t queue_capacity = 2);

    ~TwitchTestMessageWorker() override;

    TwitchTestMessageWorker(const TwitchTestMessageWorker&) = delete;
    TwitchTestMessageWorker& operator=(const TwitchTestMessageWorker&) = delete;

    [[nodiscard]] std::expected<void, ports::TwitchTestMessageDispatchError>
    enqueue(ports::TwitchTestMessageRequest request) override;
    [[nodiscard]] std::optional<ports::TwitchTestMessageResult> try_take_result() override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::optional<ports::TwitchTestMessageResult>
    wait_for_result(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;

  private:
    TwitchTestMessageWorker(const ITwitchClient& client,
                            const ports::ITwitchDeliverySessionAccess& session_access,
                            std::size_t queue_capacity) noexcept;

    void run(std::stop_token stop_token);
    [[nodiscard]] ports::TwitchTestMessageResult execute(ports::TwitchTestMessageRequest request,
                                                         const std::stop_token& stop_token) const;
    [[nodiscard]] std::optional<ports::TwitchTestMessageResult> take_result_locked();

    TwitchChatDelivery delivery_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ports::TwitchTestMessageRequest> requests_;
    std::deque<ports::TwitchTestMessageResult> results_;
    bool stopping_{};
    std::jthread thread_;
};

} // namespace manny_uploader::providers
