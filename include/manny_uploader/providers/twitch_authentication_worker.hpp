#pragma once

#include "manny_uploader/ports/twitch_authenticator.hpp"
#include "manny_uploader/providers/twitch_client.hpp"

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

enum class TwitchAuthenticationWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    ThreadStartFailed,
};

struct TwitchAuthenticationWorkerError {
    TwitchAuthenticationWorkerErrorCode code;
    std::string message;
};

class TwitchAuthenticationWorker final : public ports::ITwitchAuthenticator {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<TwitchAuthenticationWorker>,
                                       TwitchAuthenticationWorkerError>
    create(const ITwitchClient& client, std::size_t queue_capacity = 2);

    ~TwitchAuthenticationWorker() override;

    TwitchAuthenticationWorker(const TwitchAuthenticationWorker&) = delete;
    TwitchAuthenticationWorker& operator=(const TwitchAuthenticationWorker&) = delete;

    [[nodiscard]] std::expected<void, ports::TwitchAuthenticationDispatchError>
    enqueue(ports::TwitchAuthenticationRequest request) override;
    [[nodiscard]] std::optional<ports::TwitchAuthenticationResult> try_take_result() override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::optional<ports::TwitchAuthenticationResult>
    wait_for_result(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;

  private:
    TwitchAuthenticationWorker(const ITwitchClient& client, std::size_t queue_capacity);

    void run(std::stop_token stop_token);
    [[nodiscard]] ports::TwitchAuthenticationResult
    execute(ports::TwitchAuthenticationRequest request, const std::stop_token& stop_token) const;
    [[nodiscard]] std::optional<ports::TwitchAuthenticationResult> take_result_locked();

    const ITwitchClient& client_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ports::TwitchAuthenticationRequest> requests_;
    std::deque<ports::TwitchAuthenticationResult> results_;
    bool stopping_{};
    std::jthread thread_;
};

} // namespace manny_uploader::providers
