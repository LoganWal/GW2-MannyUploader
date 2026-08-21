#include "manny_uploader/application/twitch_session_owner.hpp"

#include "manny_uploader/providers/twitch_client.hpp"

#include <limits>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

namespace manny_uploader::application {
namespace {

struct ActiveTransaction {
    std::uint64_t id;
};

[[nodiscard]] TwitchSessionOwnerError owner_error(TwitchSessionOwnerErrorCode code,
                                                  std::string message) {
    return TwitchSessionOwnerError{
        .code = code,
        .message = std::move(message),
        .configuration_error = std::nullopt,
        .session_error = std::nullopt,
    };
}

[[nodiscard]] TwitchSessionOwnerError configuration_error(TwitchSessionOwnerErrorCode code,
                                                          std::string message,
                                                          const ConfigurationError& error) {
    auto mapped = owner_error(code, std::move(message));
    mapped.configuration_error = error.code;
    return mapped;
}

[[nodiscard]] TwitchSessionOwnerError session_error(TwitchSessionOwnerErrorCode code,
                                                    std::string message,
                                                    const TwitchSessionError& error) {
    auto mapped = owner_error(code, std::move(message));
    mapped.session_error = error.code;
    return mapped;
}

[[nodiscard]] support::SecretValue clone_secret(const support::SecretValue& secret) {
    const auto bytes = secret.bytes();
    return support::SecretValue{std::vector<std::byte>{bytes.begin(), bytes.end()}};
}

[[nodiscard]] ports::TwitchDeliverySession delivery_session(const ports::TwitchSession& session,
                                                            std::uint64_t revision) {
    return ports::TwitchDeliverySession{
        .access_token = clone_secret(session.credentials.access_token),
        .authenticated_user_id = session.user_id,
        .revision = revision,
    };
}

[[nodiscard]] TwitchSessionOwnerSnapshot owner_snapshot(const ports::TwitchSession& session,
                                                        std::uint64_t revision) {
    return TwitchSessionOwnerSnapshot{
        .authenticated_user_id = session.user_id,
        .login = session.login,
        .access_expires_at = session.credentials.access_expires_at,
        .revision = revision,
    };
}

[[nodiscard]] ports::TwitchDeliverySessionError
delivery_error(ports::TwitchDeliverySessionErrorCode code, std::string detail,
               std::optional<std::chrono::seconds> retry_after = std::nullopt) {
    return ports::TwitchDeliverySessionError{
        .code = code,
        .detail = std::move(detail),
        .retry_after = retry_after,
    };
}

[[nodiscard]] ports::TwitchDeliverySessionError
from_twitch_error(const providers::TwitchError& error) {
    auto code = ports::TwitchDeliverySessionErrorCode::Failed;
    switch (error.disposition) {
    case providers::TwitchDisposition::Retry:
        code = ports::TwitchDeliverySessionErrorCode::Retry;
        break;
    case providers::TwitchDisposition::Reconnect:
        code = ports::TwitchDeliverySessionErrorCode::ReconnectRequired;
        break;
    case providers::TwitchDisposition::Failed:
        code = ports::TwitchDeliverySessionErrorCode::Failed;
        break;
    case providers::TwitchDisposition::Cancelled:
        code = ports::TwitchDeliverySessionErrorCode::Cancelled;
        break;
    }
    return delivery_error(code, error.detail, error.retry_after);
}

} // namespace

struct TwitchSessionOwnerState {
    TwitchSessionOwnerState(ConfigurationService& configuration_value,
                            const providers::ITwitchClient& client_value,
                            const ports::IClock& clock_value)
        : configuration{configuration_value}, client{client_value}, clock{clock_value} {}

    ConfigurationService& configuration;
    const providers::ITwitchClient& client;
    const ports::IClock& clock;
    mutable std::mutex mutex;
    std::optional<ports::TwitchSession> active_session;
    std::optional<ActiveTransaction> transaction;
    std::uint64_t revision{};
    std::uint64_t next_transaction_id{1};
    bool shutting_down{};
};

namespace {

[[nodiscard]] std::expected<std::uint64_t, TwitchSessionOwnerError>
start_transaction_locked(TwitchSessionOwnerState& state) {
    if (state.shutting_down || state.configuration.is_shutting_down()) {
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::ShuttingDown,
                                           "Twitch session ownership is shutting down"));
    }
    if (state.transaction) {
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::Busy,
                                           "The Twitch session is already being updated"));
    }
    if (state.next_transaction_id == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::Busy,
                                           "No more Twitch session transactions can be created"));
    }
    const auto id = state.next_transaction_id++;
    state.transaction = ActiveTransaction{.id = id};
    return id;
}

[[nodiscard]] std::expected<void, TwitchSessionOwnerError>
check_transaction_locked(const TwitchSessionOwnerState& state, std::uint64_t transaction_id) {
    if (state.shutting_down) {
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::ShuttingDown,
                                           "Twitch session ownership is shutting down"));
    }
    if (!state.transaction || state.transaction->id != transaction_id) {
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::StaleTransaction,
                                           "A stale Twitch session transaction was ignored"));
    }
    return {};
}

[[nodiscard]] std::expected<void, TwitchSessionOwnerError>
persist_locked(TwitchSessionOwnerState& state, const ports::TwitchSession& session) {
    auto encoded = encode_twitch_session(session);
    if (!encoded) {
        return std::unexpected(session_error(TwitchSessionOwnerErrorCode::EncodeFailed,
                                             "The Twitch session could not be encoded safely",
                                             encoded.error()));
    }
    auto stored = state.configuration.store_secret(ports::SecretId::TwitchOAuthSession, *encoded);
    if (!stored) {
        return std::unexpected(configuration_error(TwitchSessionOwnerErrorCode::StoreFailed,
                                                   "The Twitch session could not be saved",
                                                   stored.error()));
    }
    return {};
}

[[nodiscard]] std::expected<void, TwitchSessionOwnerError>
erase_locked(TwitchSessionOwnerState& state) {
    auto erased = state.configuration.erase_secret(ports::SecretId::TwitchOAuthSession);
    if (!erased) {
        return std::unexpected(configuration_error(TwitchSessionOwnerErrorCode::EraseFailed,
                                                   "The saved Twitch session could not be removed",
                                                   erased.error()));
    }
    return {};
}

[[nodiscard]] ports::TwitchDeliverySessionError
from_owner_error(const TwitchSessionOwnerError& error) {
    switch (error.code) {
    case TwitchSessionOwnerErrorCode::Busy:
        return delivery_error(ports::TwitchDeliverySessionErrorCode::Retry, error.message);
    case TwitchSessionOwnerErrorCode::NotConnected:
        return delivery_error(ports::TwitchDeliverySessionErrorCode::NotConnected, error.message);
    case TwitchSessionOwnerErrorCode::ShuttingDown:
        return delivery_error(ports::TwitchDeliverySessionErrorCode::Cancelled, error.message);
    case TwitchSessionOwnerErrorCode::StaleTransaction:
        return delivery_error(ports::TwitchDeliverySessionErrorCode::Retry, error.message);
    case TwitchSessionOwnerErrorCode::LoadFailed:
    case TwitchSessionOwnerErrorCode::DecodeFailed:
    case TwitchSessionOwnerErrorCode::EncodeFailed:
    case TwitchSessionOwnerErrorCode::StoreFailed:
    case TwitchSessionOwnerErrorCode::EraseFailed:
        return delivery_error(ports::TwitchDeliverySessionErrorCode::Failed, error.message);
    }
    return delivery_error(ports::TwitchDeliverySessionErrorCode::Failed, error.message);
}

struct RecoveryTransaction {
    std::uint64_t id;
    ports::TwitchSession session;
};

using RecoveryStart = std::variant<ports::TwitchDeliverySession, RecoveryTransaction>;

[[nodiscard]] std::expected<RecoveryStart, ports::TwitchDeliverySessionError>
begin_recovery(TwitchSessionOwnerState& state,
               const ports::TwitchDeliverySession& rejected_session) {
    const std::scoped_lock lock{state.mutex};
    if (state.shutting_down) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::Cancelled,
                                              "Twitch session ownership is shutting down"));
    }
    if (state.transaction) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::Retry,
                                              "The Twitch session is being updated"));
    }
    if (!state.active_session) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::NotConnected,
                                              "No Twitch session is connected"));
    }
    if (rejected_session.revision != state.revision) {
        return RecoveryStart{std::in_place_type<ports::TwitchDeliverySession>,
                             delivery_session(*state.active_session, state.revision)};
    }
    if (rejected_session.authenticated_user_id != state.active_session->user_id ||
        !(rejected_session.access_token == state.active_session->credentials.access_token)) {
        return std::unexpected(
            delivery_error(ports::TwitchDeliverySessionErrorCode::ReconnectRequired,
                           "The rejected Twitch lease does not match its session revision"));
    }
    auto started = start_transaction_locked(state);
    if (!started) {
        return std::unexpected(from_owner_error(started.error()));
    }
    auto session = std::move(*state.active_session);
    state.active_session.reset();
    return RecoveryStart{std::in_place_type<RecoveryTransaction>, *started, std::move(session)};
}

void restore_recovery(TwitchSessionOwnerState& state, std::uint64_t transaction_id,
                      ports::TwitchSession session) {
    const std::scoped_lock lock{state.mutex};
    if (!state.transaction || state.transaction->id != transaction_id || state.shutting_down) {
        return;
    }
    state.active_session.emplace(std::move(session));
    state.transaction.reset();
}

void discard_recovery(TwitchSessionOwnerState& state, std::uint64_t transaction_id,
                      bool erase_saved_session) {
    const std::scoped_lock lock{state.mutex};
    if (!state.transaction || state.transaction->id != transaction_id) {
        return;
    }
    state.active_session.reset();
    state.transaction.reset();
    if (erase_saved_session && !state.shutting_down) {
        [[maybe_unused]] const auto erased = erase_locked(state);
    }
}

[[nodiscard]] std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>
commit_recovery(TwitchSessionOwnerState& state, std::uint64_t transaction_id,
                ports::TwitchSession session) {
    const std::scoped_lock lock{state.mutex};
    if (auto valid = check_transaction_locked(state, transaction_id); !valid) {
        return std::unexpected(from_owner_error(valid.error()));
    }
    if (auto persisted = persist_locked(state, session); !persisted) {
        state.transaction.reset();
        state.active_session.reset();
        return std::unexpected(from_owner_error(persisted.error()));
    }
    if (state.revision < std::numeric_limits<std::uint64_t>::max()) {
        ++state.revision;
    }
    state.active_session.emplace(std::move(session));
    state.transaction.reset();
    return delivery_session(*state.active_session, state.revision);
}

[[nodiscard]] std::expected<void, ports::TwitchDeliverySessionError>
persist_replacement(TwitchSessionOwnerState& state, std::uint64_t transaction_id,
                    const ports::TwitchSession& session) {
    const std::scoped_lock lock{state.mutex};
    if (auto valid = check_transaction_locked(state, transaction_id); !valid) {
        return std::unexpected(from_owner_error(valid.error()));
    }
    if (auto persisted = persist_locked(state, session); !persisted) {
        state.transaction.reset();
        state.active_session.reset();
        return std::unexpected(from_owner_error(persisted.error()));
    }
    return {};
}

[[nodiscard]] std::expected<void, ports::TwitchDeliverySessionError>
apply_validated_identity(ports::TwitchSession& session, providers::TwitchValidatedIdentity identity,
                         const ports::IClock& clock, bool refreshed) {
    if (identity.user_id != session.user_id) {
        return std::unexpected(delivery_error(
            ports::TwitchDeliverySessionErrorCode::ReconnectRequired,
            refreshed ? "The refreshed Twitch session changed broadcaster identity; connect again"
                      : "The Twitch session changed broadcaster identity; connect again"));
    }
    session.login = std::move(identity.login);
    session.scopes = std::move(identity.scopes);
    session.credentials.access_expires_at = clock.system_now() + identity.expires_in;
    return {};
}

[[nodiscard]] bool may_restore(providers::TwitchDisposition disposition) noexcept {
    return disposition == providers::TwitchDisposition::Retry ||
           disposition == providers::TwitchDisposition::Cancelled;
}

[[nodiscard]] std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>
recover_current(TwitchSessionOwnerState& state, RecoveryTransaction transaction,
                const std::stop_token& stop_token) {
    auto validated = state.client.validate_access_token(
        transaction.session.credentials.access_token, stop_token);
    if (validated) {
        auto applied = apply_validated_identity(transaction.session, std::move(*validated),
                                                state.clock, false);
        if (!applied) {
            discard_recovery(state, transaction.id, true);
            return std::unexpected(std::move(applied.error()));
        }
        return commit_recovery(state, transaction.id, std::move(transaction.session));
    }
    if (validated.error().disposition != providers::TwitchDisposition::Reconnect) {
        auto error = from_twitch_error(validated.error());
        if (may_restore(validated.error().disposition)) {
            restore_recovery(state, transaction.id, std::move(transaction.session));
        } else {
            discard_recovery(state, transaction.id, false);
        }
        return std::unexpected(std::move(error));
    }

    auto refreshed = state.client.refresh_access_token(
        transaction.session.credentials.refresh_token, stop_token);
    if (!refreshed) {
        auto error = from_twitch_error(refreshed.error());
        if (may_restore(refreshed.error().disposition)) {
            restore_recovery(state, transaction.id, std::move(transaction.session));
        } else {
            discard_recovery(state, transaction.id, true);
        }
        return std::unexpected(std::move(error));
    }

    transaction.session.credentials = ports::TwitchCredentialSet{
        .access_token = std::move(refreshed->access_token),
        .refresh_token = std::move(refreshed->refresh_token),
        .access_expires_at = state.clock.system_now() + refreshed->expires_in,
    };
    transaction.session.scopes = std::move(refreshed->scopes);
    if (auto persisted = persist_replacement(state, transaction.id, transaction.session);
        !persisted) {
        return std::unexpected(std::move(persisted.error()));
    }

    auto replacement = state.client.validate_access_token(
        transaction.session.credentials.access_token, stop_token);
    if (!replacement) {
        auto error = from_twitch_error(replacement.error());
        discard_recovery(state, transaction.id, true);
        return std::unexpected(std::move(error));
    }
    auto applied =
        apply_validated_identity(transaction.session, std::move(*replacement), state.clock, true);
    if (!applied) {
        discard_recovery(state, transaction.id, true);
        return std::unexpected(std::move(applied.error()));
    }
    return commit_recovery(state, transaction.id, std::move(transaction.session));
}

} // namespace

TwitchSessionOwner::TwitchSessionOwner(ConfigurationService& configuration,
                                       const providers::ITwitchClient& client,
                                       const ports::IClock& clock)
    : state_{std::make_unique<TwitchSessionOwnerState>(configuration, client, clock)} {}

TwitchSessionOwner::~TwitchSessionOwner() = default;
TwitchSessionOwner::TwitchSessionOwner(TwitchSessionOwner&&) noexcept = default;
TwitchSessionOwner& TwitchSessionOwner::operator=(TwitchSessionOwner&&) noexcept = default;

std::expected<std::optional<TwitchSessionTransaction>, TwitchSessionOwnerError>
TwitchSessionOwner::checkout_saved() {
    const std::scoped_lock lock{state_->mutex};
    auto transaction_id = start_transaction_locked(*state_);
    if (!transaction_id) {
        return std::unexpected(std::move(transaction_id.error()));
    }
    if (state_->active_session) {
        state_->transaction.reset();
        return std::unexpected(
            owner_error(TwitchSessionOwnerErrorCode::Busy, "A Twitch session is already active"));
    }

    auto encoded = state_->configuration.load_secret(ports::SecretId::TwitchOAuthSession);
    if (!encoded) {
        state_->transaction.reset();
        if (encoded.error().secret_error == ports::SecretStoreErrorCode::NotFound) {
            return std::optional<TwitchSessionTransaction>{};
        }
        return std::unexpected(configuration_error(TwitchSessionOwnerErrorCode::LoadFailed,
                                                   "The saved Twitch session could not be loaded",
                                                   encoded.error()));
    }
    auto decoded = decode_twitch_session(*encoded);
    if (!decoded) {
        state_->transaction.reset();
        return std::unexpected(session_error(TwitchSessionOwnerErrorCode::DecodeFailed,
                                             "The saved Twitch session is invalid",
                                             decoded.error()));
    }
    return std::optional<TwitchSessionTransaction>{TwitchSessionTransaction{
        .id = *transaction_id,
        .session = std::move(*decoded),
    }};
}

std::expected<TwitchSessionTransaction, TwitchSessionOwnerError>
TwitchSessionOwner::begin_new(ports::TwitchSession session) {
    const std::scoped_lock lock{state_->mutex};
    if (state_->active_session) {
        return std::unexpected(
            owner_error(TwitchSessionOwnerErrorCode::Busy, "A Twitch session is already active"));
    }
    auto transaction_id = start_transaction_locked(*state_);
    if (!transaction_id) {
        return std::unexpected(std::move(transaction_id.error()));
    }
    return TwitchSessionTransaction{
        .id = *transaction_id,
        .session = std::move(session),
    };
}

std::expected<TwitchSessionTransaction, TwitchSessionOwnerError>
TwitchSessionOwner::checkout_active() {
    const std::scoped_lock lock{state_->mutex};
    auto transaction_id = start_transaction_locked(*state_);
    if (!transaction_id) {
        return std::unexpected(std::move(transaction_id.error()));
    }
    if (!state_->active_session) {
        state_->transaction.reset();
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::NotConnected,
                                           "No Twitch session is connected"));
    }
    auto session = std::move(*state_->active_session);
    state_->active_session.reset();
    return TwitchSessionTransaction{
        .id = *transaction_id,
        .session = std::move(session),
    };
}

std::expected<void, TwitchSessionOwnerError>
TwitchSessionOwner::persist_pending(std::uint64_t transaction_id,
                                    const ports::TwitchSession& session) {
    const std::scoped_lock lock{state_->mutex};
    if (auto valid = check_transaction_locked(*state_, transaction_id); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return persist_locked(*state_, session);
}

std::expected<TwitchSessionOwnerSnapshot, TwitchSessionOwnerError>
TwitchSessionOwner::commit(std::uint64_t transaction_id, ports::TwitchSession session) {
    const std::scoped_lock lock{state_->mutex};
    if (auto valid = check_transaction_locked(*state_, transaction_id); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (auto persisted = persist_locked(*state_, session); !persisted) {
        return std::unexpected(std::move(persisted.error()));
    }
    if (state_->revision < std::numeric_limits<std::uint64_t>::max()) {
        ++state_->revision;
    }
    state_->active_session.emplace(std::move(session));
    state_->transaction.reset();
    return owner_snapshot(*state_->active_session, state_->revision);
}

std::expected<void, TwitchSessionOwnerError>
TwitchSessionOwner::restore(std::uint64_t transaction_id, ports::TwitchSession session) {
    const std::scoped_lock lock{state_->mutex};
    if (auto valid = check_transaction_locked(*state_, transaction_id); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    state_->active_session.emplace(std::move(session));
    state_->transaction.reset();
    return {};
}

std::expected<void, TwitchSessionOwnerError>
TwitchSessionOwner::discard(std::uint64_t transaction_id, bool erase_saved_session) {
    const std::scoped_lock lock{state_->mutex};
    if (auto valid = check_transaction_locked(*state_, transaction_id); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    state_->active_session.reset();
    state_->transaction.reset();
    if (erase_saved_session) {
        return erase_locked(*state_);
    }
    return {};
}

std::expected<void, TwitchSessionOwnerError> TwitchSessionOwner::erase_saved() {
    const std::scoped_lock lock{state_->mutex};
    if (state_->transaction) {
        return std::unexpected(owner_error(TwitchSessionOwnerErrorCode::Busy,
                                           "The Twitch session is already being updated"));
    }
    state_->active_session.reset();
    return erase_locked(*state_);
}

std::optional<TwitchSessionOwnerSnapshot> TwitchSessionOwner::snapshot() const {
    const std::scoped_lock lock{state_->mutex};
    if (!state_->active_session || state_->transaction) {
        return std::nullopt;
    }
    return owner_snapshot(*state_->active_session, state_->revision);
}

std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>
TwitchSessionOwner::acquire(const std::stop_token& stop_token) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::Cancelled,
                                              "Twitch delivery was cancelled"));
    }
    const std::scoped_lock lock{state_->mutex};
    if (state_->shutting_down) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::Cancelled,
                                              "Twitch session ownership is shutting down"));
    }
    if (state_->transaction) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::Retry,
                                              "The Twitch session is being updated"));
    }
    if (!state_->active_session) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::NotConnected,
                                              "No Twitch session is connected"));
    }
    return delivery_session(*state_->active_session, state_->revision);
}

std::expected<ports::TwitchDeliverySession, ports::TwitchDeliverySessionError>
TwitchSessionOwner::recover(ports::TwitchDeliverySession rejected_session,
                            const std::stop_token& stop_token) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(delivery_error(ports::TwitchDeliverySessionErrorCode::Cancelled,
                                              "Twitch delivery was cancelled"));
    }

    auto started = begin_recovery(*state_, rejected_session);
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    if (auto* current = std::get_if<ports::TwitchDeliverySession>(&*started)) {
        return std::move(*current);
    }
    return recover_current(*state_, std::get<RecoveryTransaction>(std::move(*started)), stop_token);
}

void TwitchSessionOwner::shutdown() noexcept {
    const std::scoped_lock lock{state_->mutex};
    if (state_->shutting_down) {
        return;
    }
    state_->shutting_down = true;
    state_->active_session.reset();
    state_->transaction.reset();
}

bool TwitchSessionOwner::is_shutting_down() const noexcept {
    const std::scoped_lock lock{state_->mutex};
    return state_->shutting_down;
}

} // namespace manny_uploader::application
