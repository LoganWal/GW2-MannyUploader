#include "manny_uploader/application/twitch_authentication_controller.hpp"

#include <chrono>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>

namespace manny_uploader::application {
namespace {

using namespace std::chrono_literals;

constexpr auto validation_interval = 1h;
constexpr auto refresh_margin = 5min;
constexpr auto default_retry_delay = 30s;

[[nodiscard]] TwitchAuthenticationControllerError
make_error(TwitchAuthenticationControllerErrorCode code, std::string message) {
    return TwitchAuthenticationControllerError{
        .code = code,
        .message = std::move(message),
        .configuration_error = std::nullopt,
        .session_error = std::nullopt,
        .authentication_error = std::nullopt,
    };
}

[[nodiscard]] TwitchAuthenticationControllerError
from_configuration_error(TwitchAuthenticationControllerErrorCode code, std::string message,
                         const ConfigurationError& error) {
    auto result = make_error(code, std::move(message));
    result.configuration_error = error.code;
    return result;
}

[[nodiscard]] TwitchAuthenticationControllerError
from_owner_error(const TwitchSessionOwnerError& error) {
    auto code = TwitchAuthenticationControllerErrorCode::InvalidState;
    switch (error.code) {
    case TwitchSessionOwnerErrorCode::Busy:
        code = TwitchAuthenticationControllerErrorCode::Busy;
        break;
    case TwitchSessionOwnerErrorCode::NotConnected:
        code = TwitchAuthenticationControllerErrorCode::InvalidState;
        break;
    case TwitchSessionOwnerErrorCode::LoadFailed:
        code = TwitchAuthenticationControllerErrorCode::SessionLoadFailed;
        break;
    case TwitchSessionOwnerErrorCode::DecodeFailed:
        code = TwitchAuthenticationControllerErrorCode::SessionDecodeFailed;
        break;
    case TwitchSessionOwnerErrorCode::EncodeFailed:
    case TwitchSessionOwnerErrorCode::StoreFailed:
        code = TwitchAuthenticationControllerErrorCode::SessionStoreFailed;
        break;
    case TwitchSessionOwnerErrorCode::EraseFailed:
        code = TwitchAuthenticationControllerErrorCode::SessionEraseFailed;
        break;
    case TwitchSessionOwnerErrorCode::StaleTransaction:
        code = TwitchAuthenticationControllerErrorCode::StaleResult;
        break;
    case TwitchSessionOwnerErrorCode::ShuttingDown:
        code = TwitchAuthenticationControllerErrorCode::ShuttingDown;
        break;
    }
    auto mapped = make_error(code, error.message);
    mapped.configuration_error = error.configuration_error;
    mapped.session_error = error.session_error;
    return mapped;
}

} // namespace

std::expected<TwitchAuthenticationController, TwitchAuthenticationControllerError>
TwitchAuthenticationController::create(ConfigurationService& configuration,
                                       ports::ITwitchAuthenticator& authenticator,
                                       TwitchSessionOwner& session_owner,
                                       const ports::IClock& clock) {
    if (configuration.is_shutting_down() || session_owner.is_shutting_down()) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::ShuttingDown,
                                          "Twitch authentication is shutting down"));
    }
    return TwitchAuthenticationController{
        configuration,
        authenticator,
        session_owner,
        clock,
        TwitchConnectionSnapshot{
            .state = TwitchConnectionState::Disconnected,
            .login = std::nullopt,
            .user_code = std::nullopt,
            .verification_uri = std::nullopt,
            .authorization_expires_at = std::nullopt,
            .access_expires_at = std::nullopt,
            .diagnostic = {},
            .revision = 1,
            .shutting_down = false,
        },
    };
}

TwitchAuthenticationController::TwitchAuthenticationController(
    ConfigurationService& configuration, ports::ITwitchAuthenticator& authenticator,
    TwitchSessionOwner& session_owner, const ports::IClock& clock,
    TwitchConnectionSnapshot snapshot)
    : configuration_{configuration}, authenticator_{authenticator}, session_owner_{session_owner},
      clock_{clock}, snapshot_{std::move(snapshot)} {}

TwitchConnectionSnapshot TwitchAuthenticationController::snapshot() const {
    return snapshot_;
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::begin_connection() {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::ShuttingDown,
                                          "Twitch authentication is shutting down"));
    }
    if (in_flight_ || pending_authorization_ ||
        snapshot_.state == TwitchConnectionState::Connected ||
        snapshot_.state == TwitchConnectionState::Disconnecting) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::Busy,
                                          "Twitch authentication is already active"));
    }

    reset_transient();
    return dispatch(ports::TwitchStartAuthentication{}, Flow::Start,
                    TwitchConnectionState::Starting);
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::begin_saved_connection() {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::ShuttingDown,
                                          "Twitch authentication is shutting down"));
    }
    if (in_flight_ || pending_authorization_ ||
        snapshot_.state == TwitchConnectionState::Connected ||
        snapshot_.state == TwitchConnectionState::Disconnecting) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::Busy,
                                          "Twitch authentication is already active"));
    }

    reset_transient();
    auto checked_out = session_owner_.checkout_saved();
    if (!checked_out) {
        return std::unexpected(publish_error(from_owner_error(checked_out.error())));
    }
    if (!*checked_out) {
        snapshot_.state = TwitchConnectionState::Disconnected;
        snapshot_.diagnostic.clear();
        advance_revision();
        return false;
    }
    adopt_transaction(std::move(**checked_out));
    auto queued = dispatch_validation(Flow::ValidateRestore);
    if (!queued) {
        return std::unexpected(std::move(queued.error()));
    }
    return true;
}

std::expected<bool, TwitchAuthenticationControllerError> TwitchAuthenticationController::tick() {
    if (snapshot_.shutting_down) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::ShuttingDown,
                                          "Twitch authentication is shutting down"));
    }

    if (auto result = authenticator_.try_take_result()) {
        return handle_result(std::move(*result));
    }
    if (in_flight_) {
        return false;
    }

    const auto steady_now = clock_.steady_now();
    if (pending_authorization_) {
        if (steady_now >= pending_authorization_->expires_at) {
            clear_authorization();
            pending_authorization_.reset();
            return std::unexpected(publish_error(
                make_error(TwitchAuthenticationControllerErrorCode::AuthorizationExpired,
                           "The Twitch authorization code expired; connect again")));
        }
        if (steady_now >= pending_authorization_->next_poll_at) {
            auto device_code = std::move(pending_authorization_->device_code);
            return dispatch(ports::TwitchPollAuthentication{.device_code = std::move(device_code)},
                            Flow::Poll, TwitchConnectionState::AwaitingUser)
                .transform([] { return true; });
        }
        return false;
    }

    if (deferred_action_ && steady_now >= deferred_action_->not_before) {
        const auto action = *deferred_action_;
        deferred_action_.reset();
        if (action.flow == Flow::ValidateInitial || action.flow == Flow::ValidateRestore ||
            action.flow == Flow::ValidateHourly || action.flow == Flow::ValidateAfterRefresh) {
            return dispatch_validation(action.flow).transform([] { return true; });
        }
        return dispatch_refresh(action.flow).transform([] { return true; });
    }

    if (snapshot_.state != TwitchConnectionState::Connected) {
        return false;
    }
    const auto connected = session_owner_.snapshot();
    if (!connected) {
        return false;
    }
    if (clock_.system_now() + refresh_margin >= connected->access_expires_at &&
        steady_now >= refresh_not_before_) {
        return dispatch_refresh(Flow::RefreshScheduled).transform([] { return true; });
    }
    if (steady_now >= next_validation_at_) {
        return dispatch_validation(Flow::ValidateHourly).transform([] { return true; });
    }
    return false;
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::disconnect() {
    if (snapshot_.shutting_down || configuration_.is_shutting_down()) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::ShuttingDown,
                                          "Twitch authentication is shutting down"));
    }
    if (in_flight_) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::Busy,
                                          "Twitch authentication is busy"));
    }

    auto settings = configuration_.snapshot().settings;
    settings.twitch.enabled = false;
    if (auto saved = configuration_.save_settings(std::move(settings)); !saved) {
        return std::unexpected(publish_error(
            from_configuration_error(TwitchAuthenticationControllerErrorCode::SettingsSaveFailed,
                                     "Twitch could not be disabled safely", saved.error()),
            false));
    }

    pending_authorization_.reset();
    deferred_action_.reset();
    clear_authorization();
    if (!session_) {
        auto checked_out = session_owner_.checkout_active();
        if (checked_out) {
            adopt_transaction(std::move(*checked_out));
        } else if (checked_out.error().code != TwitchSessionOwnerErrorCode::NotConnected) {
            return std::unexpected(from_owner_error(checked_out.error()));
        }
    }
    if (session_ && !session_->credentials.access_token.empty()) {
        auto access_token = std::move(session_->credentials.access_token);
        discard_transaction_best_effort(false);
        session_.reset();
        clear_public_identity();
        auto dispatched =
            dispatch(ports::TwitchRevokeAuthentication{.access_token = std::move(access_token)},
                     Flow::Revoke, TwitchConnectionState::Disconnecting);
        if (!dispatched) {
            auto dispatch_error = std::move(dispatched.error());
            auto finished =
                finish_disconnect("Disconnected locally; Twitch revocation could not be started");
            if (!finished) {
                return std::unexpected(std::move(finished.error()));
            }
            return std::unexpected(std::move(dispatch_error));
        }
        return {};
    }

    discard_transaction_best_effort(false);
    session_.reset();
    clear_public_identity();
    return finish_disconnect({});
}

void TwitchAuthenticationController::shutdown() noexcept {
    if (snapshot_.shutting_down) {
        return;
    }
    reset_transient();
    session_owner_.shutdown();
    snapshot_.state = TwitchConnectionState::ShuttingDown;
    snapshot_.diagnostic.clear();
    snapshot_.shutting_down = true;
    authenticator_.cancel_pending();
    advance_revision();
}

bool TwitchAuthenticationController::is_shutting_down() const noexcept {
    return snapshot_.shutting_down;
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::dispatch(ports::TwitchAuthenticationCommand command, Flow flow,
                                         TwitchConnectionState visible_state) {
    if (in_flight_) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::Busy,
                                          "Twitch authentication is busy"));
    }
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                     "No more Twitch authentication requests can be created")));
    }

    const auto request_id = next_request_id_++;
    const auto operation = ports::authentication_operation(command);
    auto queued = authenticator_.enqueue(ports::TwitchAuthenticationRequest{
        .request_id = request_id,
        .command = std::move(command),
    });
    if (!queued) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::DispatchFailed,
                                     "Twitch authentication work could not be started")));
    }

    in_flight_.emplace(InFlight{
        .request_id = request_id,
        .operation = operation,
        .flow = flow,
    });
    snapshot_.state = visible_state;
    snapshot_.diagnostic.clear();
    advance_revision();
    return {};
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::dispatch_validation(Flow flow) {
    if (!session_) {
        if (auto checked_out = checkout_active_session(); !checked_out) {
            return std::unexpected(std::move(checked_out.error()));
        }
    }
    if (!session_ || session_->credentials.access_token.empty() ||
        session_->credentials.refresh_token.empty()) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                     "The Twitch session is unavailable for validation")));
    }
    auto credentials = std::move(session_->credentials);
    return dispatch(ports::TwitchValidateAuthentication{.credentials = std::move(credentials)},
                    flow, TwitchConnectionState::Validating);
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::dispatch_refresh(Flow flow) {
    if (!session_) {
        if (auto checked_out = checkout_active_session(); !checked_out) {
            return std::unexpected(std::move(checked_out.error()));
        }
    }
    if (!session_ || session_->credentials.access_token.empty() ||
        session_->credentials.refresh_token.empty() || session_->user_id.empty()) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                     "The Twitch session is unavailable for refresh")));
    }
    auto credentials = std::move(session_->credentials);
    return dispatch(ports::TwitchRefreshAuthentication{.credentials = std::move(credentials)}, flow,
                    TwitchConnectionState::Refreshing);
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_result(ports::TwitchAuthenticationResult result) {
    if (!in_flight_ || result.request_id != in_flight_->request_id) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::StaleResult,
                                          "A stale Twitch authentication result was ignored"));
    }
    if (result.operation != in_flight_->operation) {
        in_flight_.reset();
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidResult,
                                     "Twitch authentication returned an unexpected operation")));
    }

    const auto flow = in_flight_->flow;
    in_flight_.reset();
    if (!result.outcome) {
        return handle_failure(flow, std::move(result.outcome.error()));
    }
    return handle_success(flow, std::move(*result.outcome));
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_success(Flow flow,
                                               ports::TwitchAuthenticationSuccess success) {
    if (flow == Flow::Start) {
        auto* started = std::get_if<ports::TwitchAuthorizationStarted>(&success);
        if (started == nullptr) {
            return std::unexpected(
                publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidResult,
                                         "Twitch authorization returned an unexpected result")));
        }
        const auto steady_now = clock_.steady_now();
        pending_authorization_.emplace(PendingAuthorization{
            .device_code = std::move(started->device_code),
            .expires_at = steady_now + started->expires_in,
            .next_poll_at = steady_now + started->polling_interval,
            .polling_interval = started->polling_interval,
        });
        snapshot_.state = TwitchConnectionState::AwaitingUser;
        snapshot_.user_code = std::move(started->user_code);
        snapshot_.verification_uri = std::move(started->verification_uri);
        snapshot_.authorization_expires_at = clock_.system_now() + started->expires_in;
        snapshot_.diagnostic.clear();
        advance_revision();
        return true;
    }

    if (flow == Flow::Poll) {
        if (auto* pending = std::get_if<ports::TwitchAuthorizationPending>(&success)) {
            if (!pending_authorization_) {
                return std::unexpected(
                    publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                             "Twitch authorization state was lost")));
            }
            pending_authorization_->device_code = std::move(pending->device_code);
            pending_authorization_->next_poll_at =
                clock_.steady_now() + pending_authorization_->polling_interval;
            snapshot_.state = TwitchConnectionState::AwaitingUser;
            snapshot_.diagnostic.clear();
            advance_revision();
            return true;
        }
        auto* granted = std::get_if<ports::TwitchAuthorizationGranted>(&success);
        if (granted == nullptr) {
            return std::unexpected(
                publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidResult,
                                         "Twitch authorization returned an unexpected result")));
        }
        auto transaction = session_owner_.begin_new(ports::TwitchSession{
            .credentials =
                ports::TwitchCredentialSet{
                    .access_token = std::move(granted->access_token),
                    .refresh_token = std::move(granted->refresh_token),
                    .access_expires_at = clock_.system_now() + granted->expires_in,
                },
            .user_id = {},
            .login = {},
            .scopes = std::move(granted->scopes),
        });
        if (!transaction) {
            return std::unexpected(publish_error(from_owner_error(transaction.error())));
        }
        adopt_transaction(std::move(*transaction));
        pending_authorization_.reset();
        clear_authorization();
        auto queued = dispatch_validation(Flow::ValidateInitial);
        if (!queued) {
            return std::unexpected(std::move(queued.error()));
        }
        return true;
    }

    if (flow == Flow::ValidateInitial || flow == Flow::ValidateRestore ||
        flow == Flow::ValidateHourly || flow == Flow::ValidateAfterRefresh) {
        auto* validated = std::get_if<ports::TwitchValidationSucceeded>(&success);
        if (validated == nullptr) {
            return std::unexpected(
                publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidResult,
                                         "Twitch validation returned an unexpected result")));
        }
        return handle_validation(flow, std::move(*validated));
    }

    if (flow == Flow::RefreshRestore || flow == Flow::RefreshScheduled ||
        flow == Flow::RefreshRecovery) {
        auto* refreshed = std::get_if<ports::TwitchRefreshSucceeded>(&success);
        if (refreshed == nullptr) {
            return std::unexpected(
                publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidResult,
                                         "Twitch refresh returned an unexpected result")));
        }
        return handle_refresh(flow, std::move(*refreshed));
    }

    if (!std::holds_alternative<ports::TwitchRevocationSucceeded>(success)) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidResult,
                                     "Twitch disconnect returned an unexpected result")));
    }
    auto finished = finish_disconnect({});
    if (!finished) {
        return std::unexpected(std::move(finished.error()));
    }
    return true;
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_failure(Flow flow,
                                               ports::TwitchAuthenticationFailure failure) {
    if (flow == Flow::ValidateInitial || flow == Flow::ValidateRestore ||
        flow == Flow::ValidateHourly || flow == Flow::ValidateAfterRefresh) {
        return handle_validation_failure(flow, std::move(failure));
    }
    if (flow == Flow::RefreshRestore || flow == Flow::RefreshScheduled ||
        flow == Flow::RefreshRecovery) {
        return handle_refresh_failure(flow, std::move(failure));
    }
    if (flow == Flow::Revoke) {
        return handle_revoke_failure(std::move(failure));
    }
    if (flow == Flow::Poll) {
        return handle_poll_failure(std::move(failure));
    }
    return handle_terminal_failure(std::move(failure));
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_poll_failure(ports::TwitchAuthenticationFailure failure) {
    if (failure.code != ports::TwitchAuthenticationFailureCode::Retry || !failure.device_code ||
        !pending_authorization_) {
        return handle_terminal_failure(std::move(failure));
    }
    pending_authorization_.value().device_code = std::move(failure.device_code.value());
    pending_authorization_.value().next_poll_at =
        clock_.steady_now() + failure.retry_after.value_or(default_retry_delay);
    snapshot_.state = TwitchConnectionState::AwaitingUser;
    snapshot_.diagnostic =
        failure.detail.empty() ? "Twitch authorization will retry" : std::move(failure.detail);
    advance_revision();
    return true;
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_validation_failure(
    Flow flow, ports::TwitchAuthenticationFailure failure) {
    if (failure.credentials && session_) {
        session_.value().credentials = std::move(failure.credentials.value());
    }
    if (failure.code == ports::TwitchAuthenticationFailureCode::Retry && session_) {
        deferred_action_.emplace(DeferredAction{
            .flow = flow,
            .not_before = clock_.steady_now() + failure.retry_after.value_or(default_retry_delay),
        });
        snapshot_.state = TwitchConnectionState::Validating;
        snapshot_.diagnostic =
            failure.detail.empty() ? "Twitch validation will retry" : std::move(failure.detail);
        advance_revision();
        return true;
    }
    const auto may_refresh = flow == Flow::ValidateRestore || flow == Flow::ValidateHourly;
    if (failure.code == ports::TwitchAuthenticationFailureCode::Reconnect && may_refresh &&
        session_) {
        const auto refresh_flow =
            flow == Flow::ValidateRestore ? Flow::RefreshRestore : Flow::RefreshRecovery;
        auto queued = dispatch_refresh(refresh_flow);
        if (!queued) {
            return std::unexpected(std::move(queued.error()));
        }
        return true;
    }
    if (failure.code == ports::TwitchAuthenticationFailureCode::Reconnect &&
        flow == Flow::ValidateAfterRefresh) {
        erase_invalid_session_best_effort();
    }
    return handle_terminal_failure(std::move(failure));
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_refresh_failure(Flow flow,
                                                       ports::TwitchAuthenticationFailure failure) {
    if (failure.credentials && session_) {
        session_.value().credentials = std::move(failure.credentials.value());
    }
    if (failure.code != ports::TwitchAuthenticationFailureCode::Retry || !session_) {
        erase_invalid_session_best_effort();
        return handle_terminal_failure(std::move(failure));
    }
    const auto retry_at = clock_.steady_now() + failure.retry_after.value_or(default_retry_delay);
    deferred_action_.emplace(DeferredAction{.flow = flow, .not_before = retry_at});
    refresh_not_before_ = retry_at;
    snapshot_.state = TwitchConnectionState::Refreshing;
    snapshot_.diagnostic =
        failure.detail.empty() ? "Twitch refresh will retry" : std::move(failure.detail);
    advance_revision();
    return true;
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_revoke_failure(ports::TwitchAuthenticationFailure failure) {
    auto finished = finish_disconnect("Disconnected locally; Twitch revocation was not confirmed");
    if (!finished) {
        return std::unexpected(std::move(finished.error()));
    }
    auto error = make_error(TwitchAuthenticationControllerErrorCode::AuthenticationFailed,
                            "Twitch revocation was not confirmed");
    error.authentication_error = failure.code;
    return std::unexpected(std::move(error));
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_terminal_failure(
    ports::TwitchAuthenticationFailure failure) {
    auto error = make_error(failure.code == ports::TwitchAuthenticationFailureCode::Cancelled
                                ? TwitchAuthenticationControllerErrorCode::AuthenticationCancelled
                                : TwitchAuthenticationControllerErrorCode::AuthenticationFailed,
                            failure.detail.empty() ? "Twitch authentication failed"
                                                   : std::move(failure.detail));
    error.authentication_error = failure.code;
    return std::unexpected(publish_error(std::move(error)));
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_validation([[maybe_unused]] Flow flow,
                                                  ports::TwitchValidationSucceeded validated) {
    if (!session_ || !session_transaction_id_) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                     "Twitch validation completed without a session")));
    }
    auto& current = session_.value();
    if (!current.user_id.empty() && current.user_id != validated.user_id) {
        erase_invalid_session_best_effort();
        return std::unexpected(publish_error(
            make_error(TwitchAuthenticationControllerErrorCode::IdentityMismatch,
                       "The Twitch session changed broadcaster identity; connect again")));
    }

    current.credentials = std::move(validated.credentials);
    current.credentials.access_expires_at = clock_.system_now() + validated.expires_in;
    current.user_id = std::move(validated.user_id);
    current.login = std::move(validated.login);
    current.scopes = std::move(validated.scopes);
    const auto transaction_id = *session_transaction_id_;
    auto committed = session_owner_.commit(transaction_id, std::move(current));
    if (!committed) {
        return std::unexpected(publish_error(from_owner_error(committed.error())));
    }
    session_.reset();
    session_transaction_id_.reset();
    publish_connected(std::move(*committed));
    return true;
}

std::expected<bool, TwitchAuthenticationControllerError>
TwitchAuthenticationController::handle_refresh([[maybe_unused]] Flow flow,
                                               ports::TwitchRefreshSucceeded refreshed) {
    if (!session_ || session_->user_id.empty()) {
        return std::unexpected(
            publish_error(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                     "Twitch refresh completed without a validated identity")));
    }

    auto& current = session_.value();
    current.credentials = ports::TwitchCredentialSet{
        .access_token = std::move(refreshed.access_token),
        .refresh_token = std::move(refreshed.refresh_token),
        .access_expires_at = clock_.system_now() + refreshed.expires_in,
    };
    current.scopes = std::move(refreshed.scopes);
    auto persisted = persist_pending_session();
    if (!persisted) {
        erase_invalid_session_best_effort();
        return std::unexpected(publish_error(std::move(persisted.error())));
    }

    auto queued = dispatch_validation(Flow::ValidateAfterRefresh);
    if (!queued) {
        return std::unexpected(std::move(queued.error()));
    }
    return true;
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::persist_pending_session() {
    if (!session_ || !session_transaction_id_) {
        return std::unexpected(make_error(TwitchAuthenticationControllerErrorCode::InvalidState,
                                          "The Twitch session is unavailable"));
    }
    if (auto persisted = session_owner_.persist_pending(*session_transaction_id_, session_.value());
        !persisted) {
        return std::unexpected(from_owner_error(persisted.error()));
    }
    return {};
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::erase_session(std::string message) {
    if (auto erased = session_owner_.erase_saved(); !erased) {
        auto error = from_owner_error(erased.error());
        error.message = std::move(message);
        return std::unexpected(std::move(error));
    }
    return {};
}

void TwitchAuthenticationController::erase_invalid_session_best_effort() noexcept {
    if (session_transaction_id_) {
        [[maybe_unused]] const auto discarded =
            session_owner_.discard(*session_transaction_id_, true);
        session_transaction_id_.reset();
        session_.reset();
        return;
    }
    [[maybe_unused]] const auto erased = session_owner_.erase_saved();
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::finish_disconnect(std::string diagnostic) {
    auto erased = erase_session("The saved Twitch session could not be removed");
    if (!erased) {
        return std::unexpected(publish_error(std::move(erased.error())));
    }
    reset_transient();
    snapshot_.state = TwitchConnectionState::Disconnected;
    snapshot_.diagnostic = std::move(diagnostic);
    advance_revision();
    return {};
}

TwitchAuthenticationControllerError
TwitchAuthenticationController::publish_error(TwitchAuthenticationControllerError error,
                                              bool clear_session) {
    in_flight_.reset();
    pending_authorization_.reset();
    deferred_action_.reset();
    if (clear_session) {
        discard_transaction_best_effort(false);
        session_.reset();
    }
    clear_authorization();
    clear_public_identity();
    snapshot_.state = TwitchConnectionState::Error;
    snapshot_.diagnostic = error.message;
    advance_revision();
    return error;
}

void TwitchAuthenticationController::publish_connected(TwitchSessionOwnerSnapshot connected) {
    clear_authorization();
    deferred_action_.reset();
    snapshot_.state = TwitchConnectionState::Connected;
    snapshot_.login = std::move(connected.login);
    snapshot_.access_expires_at = connected.access_expires_at;
    snapshot_.diagnostic.clear();
    const auto steady_now = clock_.steady_now();
    next_validation_at_ = steady_now + validation_interval;
    refresh_not_before_ = steady_now;
    advance_revision();
}

std::expected<void, TwitchAuthenticationControllerError>
TwitchAuthenticationController::checkout_active_session() {
    auto checked_out = session_owner_.checkout_active();
    if (!checked_out) {
        return std::unexpected(from_owner_error(checked_out.error()));
    }
    adopt_transaction(std::move(*checked_out));
    return {};
}

void TwitchAuthenticationController::adopt_transaction(TwitchSessionTransaction transaction) {
    session_transaction_id_ = transaction.id;
    session_.emplace(std::move(transaction.session));
}

void TwitchAuthenticationController::discard_transaction_best_effort(
    bool erase_saved_session) noexcept {
    if (!session_transaction_id_) {
        return;
    }
    [[maybe_unused]] const auto discarded =
        session_owner_.discard(*session_transaction_id_, erase_saved_session);
    session_transaction_id_.reset();
}

void TwitchAuthenticationController::clear_public_identity() noexcept {
    snapshot_.login.reset();
    snapshot_.access_expires_at.reset();
}

void TwitchAuthenticationController::clear_authorization() noexcept {
    snapshot_.user_code.reset();
    snapshot_.verification_uri.reset();
    snapshot_.authorization_expires_at.reset();
}

void TwitchAuthenticationController::reset_transient() noexcept {
    in_flight_.reset();
    pending_authorization_.reset();
    discard_transaction_best_effort(false);
    session_.reset();
    session_transaction_id_.reset();
    deferred_action_.reset();
    clear_authorization();
    clear_public_identity();
}

void TwitchAuthenticationController::advance_revision() noexcept {
    if (snapshot_.revision < std::numeric_limits<std::uint64_t>::max()) {
        ++snapshot_.revision;
    }
}

} // namespace manny_uploader::application
