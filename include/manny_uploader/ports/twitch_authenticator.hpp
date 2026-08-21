#pragma once

#include "manny_uploader/support/secret_value.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace manny_uploader::ports {

struct TwitchCredentialSet {
    support::SecretValue access_token;
    support::SecretValue refresh_token;
    std::chrono::system_clock::time_point access_expires_at;
};

struct TwitchSession {
    TwitchCredentialSet credentials;
    std::string user_id;
    std::string login;
    std::vector<std::string> scopes;
};

enum class TwitchAuthenticationOperation : std::uint8_t {
    Start,
    Poll,
    Validate,
    Refresh,
    Revoke,
};

struct TwitchStartAuthentication {};

struct TwitchPollAuthentication {
    support::SecretValue device_code;
};

struct TwitchValidateAuthentication {
    TwitchCredentialSet credentials;
};

struct TwitchRefreshAuthentication {
    TwitchCredentialSet credentials;
};

struct TwitchRevokeAuthentication {
    support::SecretValue access_token;
};

using TwitchAuthenticationCommand =
    std::variant<TwitchStartAuthentication, TwitchPollAuthentication, TwitchValidateAuthentication,
                 TwitchRefreshAuthentication, TwitchRevokeAuthentication>;

struct TwitchAuthenticationRequest {
    std::uint64_t request_id;
    TwitchAuthenticationCommand command;
};

struct TwitchAuthorizationStarted {
    support::SecretValue device_code;
    std::string user_code;
    std::string verification_uri;
    std::chrono::seconds expires_in;
    std::chrono::seconds polling_interval;
};

struct TwitchAuthorizationPending {
    support::SecretValue device_code;
};

struct TwitchAuthorizationGranted {
    support::SecretValue access_token;
    support::SecretValue refresh_token;
    std::chrono::seconds expires_in;
    std::vector<std::string> scopes;
};

struct TwitchValidationSucceeded {
    TwitchCredentialSet credentials;
    std::string user_id;
    std::string login;
    std::chrono::seconds expires_in;
    std::vector<std::string> scopes;
};

struct TwitchRefreshSucceeded {
    support::SecretValue access_token;
    support::SecretValue refresh_token;
    std::chrono::seconds expires_in;
    std::vector<std::string> scopes;
};

struct TwitchRevocationSucceeded {};

using TwitchAuthenticationSuccess =
    std::variant<TwitchAuthorizationStarted, TwitchAuthorizationPending, TwitchAuthorizationGranted,
                 TwitchValidationSucceeded, TwitchRefreshSucceeded, TwitchRevocationSucceeded>;

enum class TwitchAuthenticationFailureCode : std::uint8_t {
    Retry,
    Reconnect,
    Failed,
    Cancelled,
};

struct TwitchAuthenticationFailure {
    TwitchAuthenticationFailureCode code;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<support::SecretValue> device_code;
    std::optional<TwitchCredentialSet> credentials;
};

struct TwitchAuthenticationResult {
    std::uint64_t request_id;
    TwitchAuthenticationOperation operation;
    std::expected<TwitchAuthenticationSuccess, TwitchAuthenticationFailure> outcome;
};

struct TwitchAuthenticationDispatchError {
    std::string message;
};

class ITwitchAuthenticator {
  public:
    virtual ~ITwitchAuthenticator() = default;

    [[nodiscard]] virtual std::expected<void, TwitchAuthenticationDispatchError>
    enqueue(TwitchAuthenticationRequest request) = 0;
    [[nodiscard]] virtual std::optional<TwitchAuthenticationResult> try_take_result() = 0;
    virtual void cancel_pending() noexcept = 0;
};

[[nodiscard]] TwitchAuthenticationOperation
authentication_operation(const TwitchAuthenticationCommand& command) noexcept;

} // namespace manny_uploader::ports
