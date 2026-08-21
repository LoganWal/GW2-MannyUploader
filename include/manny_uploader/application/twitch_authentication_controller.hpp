#pragma once

#include "manny_uploader/application/configuration_service.hpp"
#include "manny_uploader/application/twitch_session.hpp"
#include "manny_uploader/application/twitch_session_owner.hpp"
#include "manny_uploader/ports/clock.hpp"
#include "manny_uploader/ports/twitch_authenticator.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace manny_uploader::application {

enum class TwitchConnectionState : std::uint8_t {
    Disconnected,
    Starting,
    AwaitingUser,
    Validating,
    Refreshing,
    Connected,
    Disconnecting,
    Error,
    ShuttingDown,
};

struct TwitchConnectionSnapshot {
    TwitchConnectionState state;
    std::optional<std::string> login;
    std::optional<std::string> user_code;
    std::optional<std::string> verification_uri;
    std::optional<std::chrono::system_clock::time_point> authorization_expires_at;
    std::optional<std::chrono::system_clock::time_point> access_expires_at;
    std::string diagnostic;
    std::uint64_t revision;
    bool shutting_down;
};

enum class TwitchAuthenticationControllerErrorCode : std::uint8_t {
    Busy,
    InvalidState,
    DispatchFailed,
    StaleResult,
    InvalidResult,
    AuthorizationExpired,
    AuthenticationFailed,
    AuthenticationCancelled,
    SessionLoadFailed,
    SessionDecodeFailed,
    SessionStoreFailed,
    SessionEraseFailed,
    SettingsSaveFailed,
    IdentityMismatch,
    ShuttingDown,
};

struct TwitchAuthenticationControllerError {
    TwitchAuthenticationControllerErrorCode code;
    std::string message;
    std::optional<ConfigurationErrorCode> configuration_error;
    std::optional<TwitchSessionErrorCode> session_error;
    std::optional<ports::TwitchAuthenticationFailureCode> authentication_error;
};

class TwitchAuthenticationController {
  public:
    [[nodiscard]] static std::expected<TwitchAuthenticationController,
                                       TwitchAuthenticationControllerError>
    create(ConfigurationService& configuration, ports::ITwitchAuthenticator& authenticator,
           TwitchSessionOwner& session_owner, const ports::IClock& clock);

    TwitchAuthenticationController(TwitchAuthenticationController&&) noexcept = default;
    TwitchAuthenticationController& operator=(TwitchAuthenticationController&&) = delete;
    TwitchAuthenticationController(const TwitchAuthenticationController&) = delete;
    TwitchAuthenticationController& operator=(const TwitchAuthenticationController&) = delete;

    [[nodiscard]] TwitchConnectionSnapshot snapshot() const;

    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError> begin_connection();
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError> begin_saved_connection();
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError> tick();
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError> disconnect();

    void shutdown() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    enum class Flow : std::uint8_t {
        Start,
        Poll,
        ValidateInitial,
        ValidateRestore,
        ValidateHourly,
        ValidateAfterRefresh,
        RefreshRestore,
        RefreshScheduled,
        RefreshRecovery,
        Revoke,
    };

    struct InFlight {
        std::uint64_t request_id;
        ports::TwitchAuthenticationOperation operation;
        Flow flow;
    };

    struct PendingAuthorization {
        support::SecretValue device_code;
        std::chrono::steady_clock::time_point expires_at;
        std::chrono::steady_clock::time_point next_poll_at;
        std::chrono::seconds polling_interval;
    };

    struct DeferredAction {
        Flow flow;
        std::chrono::steady_clock::time_point not_before;
    };

    TwitchAuthenticationController(ConfigurationService& configuration,
                                   ports::ITwitchAuthenticator& authenticator,
                                   TwitchSessionOwner& session_owner, const ports::IClock& clock,
                                   TwitchConnectionSnapshot snapshot);

    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    dispatch(ports::TwitchAuthenticationCommand command, Flow flow,
             TwitchConnectionState visible_state);
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    dispatch_validation(Flow flow);
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    dispatch_refresh(Flow flow);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_result(ports::TwitchAuthenticationResult result);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_success(Flow flow, ports::TwitchAuthenticationSuccess success);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_failure(Flow flow, ports::TwitchAuthenticationFailure failure);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_poll_failure(ports::TwitchAuthenticationFailure failure);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_validation_failure(Flow flow, ports::TwitchAuthenticationFailure failure);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_refresh_failure(Flow flow, ports::TwitchAuthenticationFailure failure);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_revoke_failure(ports::TwitchAuthenticationFailure failure);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_terminal_failure(ports::TwitchAuthenticationFailure failure);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_validation(Flow flow, ports::TwitchValidationSucceeded validated);
    [[nodiscard]] std::expected<bool, TwitchAuthenticationControllerError>
    handle_refresh(Flow flow, ports::TwitchRefreshSucceeded refreshed);
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    persist_pending_session();
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    erase_session(std::string message);
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    finish_disconnect(std::string diagnostic);
    void erase_invalid_session_best_effort() noexcept;
    [[nodiscard]] TwitchAuthenticationControllerError
    publish_error(TwitchAuthenticationControllerError error, bool clear_session = true);
    void publish_connected(TwitchSessionOwnerSnapshot connected);
    [[nodiscard]] std::expected<void, TwitchAuthenticationControllerError>
    checkout_active_session();
    void adopt_transaction(TwitchSessionTransaction transaction);
    void discard_transaction_best_effort(bool erase_saved_session) noexcept;
    void clear_public_identity() noexcept;
    void clear_authorization() noexcept;
    void reset_transient() noexcept;
    void advance_revision() noexcept;

    ConfigurationService& configuration_;
    ports::ITwitchAuthenticator& authenticator_;
    TwitchSessionOwner& session_owner_;
    const ports::IClock& clock_;
    TwitchConnectionSnapshot snapshot_;
    std::optional<InFlight> in_flight_;
    std::optional<PendingAuthorization> pending_authorization_;
    std::optional<ports::TwitchSession> session_;
    std::optional<std::uint64_t> session_transaction_id_;
    std::optional<DeferredAction> deferred_action_;
    std::chrono::steady_clock::time_point next_validation_at_{};
    std::chrono::steady_clock::time_point refresh_not_before_{};
    std::uint64_t next_request_id_{1};
};

} // namespace manny_uploader::application
