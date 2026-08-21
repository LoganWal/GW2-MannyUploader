#pragma once

#include "manny_uploader/application/configuration_service.hpp"
#include "manny_uploader/application/twitch_session.hpp"
#include "manny_uploader/ports/clock.hpp"
#include "manny_uploader/ports/twitch_delivery_session.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace manny_uploader::providers {
class ITwitchClient;
}

namespace manny_uploader::application {

struct TwitchSessionTransaction {
    std::uint64_t id;
    ports::TwitchSession session;
};

struct TwitchSessionOwnerSnapshot {
    std::string authenticated_user_id;
    std::string login;
    std::chrono::system_clock::time_point access_expires_at;
    std::uint64_t revision;
};

enum class TwitchSessionOwnerErrorCode : std::uint8_t {
    Busy,
    NotConnected,
    LoadFailed,
    DecodeFailed,
    EncodeFailed,
    StoreFailed,
    EraseFailed,
    StaleTransaction,
    ShuttingDown,
};

struct TwitchSessionOwnerError {
    TwitchSessionOwnerErrorCode code;
    std::string message;
    std::optional<ConfigurationErrorCode> configuration_error;
    std::optional<TwitchSessionErrorCode> session_error;
};

struct TwitchSessionOwnerState;

// The single process-local owner of Twitch credentials. Authentication controller
// transactions and delivery recovery are mutually exclusive, and only a completed,
// durably stored transaction becomes visible to chat delivery.
class TwitchSessionOwner final : public ports::ITwitchDeliverySessionAccess {
  public:
    TwitchSessionOwner(ConfigurationService& configuration, const providers::ITwitchClient& client,
                       const ports::IClock& clock);
    ~TwitchSessionOwner();

    TwitchSessionOwner(TwitchSessionOwner&&) noexcept;
    TwitchSessionOwner& operator=(TwitchSessionOwner&&) noexcept;
    TwitchSessionOwner(const TwitchSessionOwner&) = delete;
    TwitchSessionOwner& operator=(const TwitchSessionOwner&) = delete;

    [[nodiscard]] std::expected<std::optional<TwitchSessionTransaction>, TwitchSessionOwnerError>
    checkout_saved();
    [[nodiscard]] std::expected<TwitchSessionTransaction, TwitchSessionOwnerError>
    begin_new(ports::TwitchSession session);
    [[nodiscard]] std::expected<TwitchSessionTransaction, TwitchSessionOwnerError>
    checkout_active();

    [[nodiscard]] std::expected<void, TwitchSessionOwnerError>
    persist_pending(std::uint64_t transaction_id, const ports::TwitchSession& session);
    [[nodiscard]] std::expected<TwitchSessionOwnerSnapshot, TwitchSessionOwnerError>
    commit(std::uint64_t transaction_id, ports::TwitchSession session);
    [[nodiscard]] std::expected<void, TwitchSessionOwnerError>
    restore(std::uint64_t transaction_id, ports::TwitchSession session);
    [[nodiscard]] std::expected<void, TwitchSessionOwnerError> discard(std::uint64_t transaction_id,
                                                                       bool erase_saved_session);
    [[nodiscard]] std::expected<void, TwitchSessionOwnerError> erase_saved();

    [[nodiscard]] std::optional<TwitchSessionOwnerSnapshot> snapshot() const;

    [[nodiscard]] std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>
    acquire(const std::stop_token& stop_token) const override;
    [[nodiscard]] std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>
    recover(ports::TwitchDeliverySession rejected_session,
            const std::stop_token& stop_token) const override;

    void shutdown() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    std::unique_ptr<TwitchSessionOwnerState> state_;
};

} // namespace manny_uploader::application
