#pragma once

#include "manny_uploader/ports/donbot_verifier.hpp"
#include "manny_uploader/providers/donbot_client.hpp"

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

enum class DonBotVerificationWorkerErrorCode : std::uint8_t {
    InvalidCapacity,
    ThreadStartFailed,
};

struct DonBotVerificationWorkerError {
    DonBotVerificationWorkerErrorCode code;
    std::string message;
};

class DonBotVerificationWorker final : public ports::IDonBotVerifier {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<DonBotVerificationWorker>,
                                       DonBotVerificationWorkerError>
    create(const IDonBotClient& client, std::size_t queue_capacity = 2);

    ~DonBotVerificationWorker() override;

    DonBotVerificationWorker(const DonBotVerificationWorker&) = delete;
    DonBotVerificationWorker& operator=(const DonBotVerificationWorker&) = delete;

    [[nodiscard]] std::expected<void, ports::DonBotVerificationDispatchError>
    enqueue(ports::DonBotVerificationRequest request) override;
    [[nodiscard]] std::optional<ports::DonBotVerificationResult> try_take_result() override;
    void cancel_pending() noexcept override;

    [[nodiscard]] std::optional<ports::DonBotVerificationResult>
    wait_for_result(std::chrono::milliseconds timeout);
    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] bool is_stopping() const noexcept;

  private:
    DonBotVerificationWorker(const IDonBotClient& client, std::size_t queue_capacity);

    void run(std::stop_token stop_token);
    [[nodiscard]] std::optional<ports::DonBotVerificationResult> take_result_locked();

    const IDonBotClient& client_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ports::DonBotVerificationRequest> requests_;
    std::deque<ports::DonBotVerificationResult> results_;
    bool stopping_{};
    std::jthread thread_;
};

} // namespace manny_uploader::providers
