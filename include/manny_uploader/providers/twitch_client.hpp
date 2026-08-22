#pragma once

#include "manny_uploader/ports/http_client.hpp"
#include "manny_uploader/support/secret_value.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manny_uploader::providers {

inline constexpr std::string_view twitch_chat_scope = "user:write:chat";
inline constexpr std::size_t max_twitch_token_bytes = std::size_t{8} * 1024U;
inline constexpr std::size_t max_twitch_chat_characters = 500;

struct TwitchDeviceAuthorization {
    support::SecretValue device_code;
    std::string user_code;
    std::string verification_uri;
    std::chrono::seconds expires_in;
    std::chrono::seconds polling_interval;
};

struct TwitchTokenGrant {
    support::SecretValue access_token;
    support::SecretValue refresh_token;
    std::chrono::seconds expires_in;
    std::vector<std::string> scopes;
};

struct TwitchAuthorizationPending {};
using TwitchDevicePollResult = std::variant<TwitchAuthorizationPending, TwitchTokenGrant>;

struct TwitchValidatedIdentity {
    std::string user_id;
    std::string login;
    std::chrono::seconds expires_in;
    std::vector<std::string> scopes;
};

struct TwitchDropReason {
    std::string code;
    std::string message;
};

struct TwitchChatResult {
    bool is_sent;
    std::optional<std::string> message_id;
    std::optional<TwitchDropReason> drop_reason;
};

enum class TwitchDisposition : std::uint8_t {
    Retry,
    Reconnect,
    Failed,
    Cancelled,
};

struct TwitchError {
    TwitchDisposition disposition;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<ports::HttpErrorCode> http_error;
    std::optional<std::uint16_t> http_status;
};

class ITwitchClient {
  public:
    virtual ~ITwitchClient() = default;

    [[nodiscard]] virtual std::expected<TwitchDeviceAuthorization, TwitchError>
    start_device_authorization(const std::stop_token& stop_token = {}) const = 0;
    [[nodiscard]] virtual std::expected<TwitchDevicePollResult, TwitchError>
    poll_device_authorization(const support::SecretValue& device_code,
                              const std::stop_token& stop_token = {}) const = 0;
    [[nodiscard]] virtual std::expected<TwitchValidatedIdentity, TwitchError>
    validate_access_token(const support::SecretValue& access_token,
                          const std::stop_token& stop_token = {}) const = 0;
    [[nodiscard]] virtual std::expected<TwitchTokenGrant, TwitchError>
    refresh_access_token(const support::SecretValue& refresh_token,
                         const std::stop_token& stop_token = {}) const = 0;
    [[nodiscard]] virtual std::expected<void, TwitchError>
    revoke_access_token(const support::SecretValue& access_token,
                        const std::stop_token& stop_token = {}) const = 0;
    [[nodiscard]] virtual std::expected<TwitchChatResult, TwitchError>
    send_chat_message(std::string_view authenticated_user_id, std::string_view message,
                      const support::SecretValue& access_token,
                      const std::stop_token& stop_token = {}) const = 0;
};

class TwitchClient final : public ITwitchClient {
  public:
    [[nodiscard]] static std::expected<TwitchClient, TwitchError>
    create(const ports::IHttpClient& http_client, std::string client_id);
    [[nodiscard]] static TwitchClient create_unconfigured(const ports::IHttpClient& http_client);

    [[nodiscard]] std::expected<void, TwitchError> update_client_id(std::string client_id);
    [[nodiscard]] bool configured() const noexcept;

    [[nodiscard]] std::expected<TwitchDeviceAuthorization, TwitchError>
    start_device_authorization(const std::stop_token& stop_token = {}) const override;
    [[nodiscard]] std::expected<TwitchDevicePollResult, TwitchError>
    poll_device_authorization(const support::SecretValue& device_code,
                              const std::stop_token& stop_token = {}) const override;
    [[nodiscard]] std::expected<TwitchValidatedIdentity, TwitchError>
    validate_access_token(const support::SecretValue& access_token,
                          const std::stop_token& stop_token = {}) const override;
    [[nodiscard]] std::expected<TwitchTokenGrant, TwitchError>
    refresh_access_token(const support::SecretValue& refresh_token,
                         const std::stop_token& stop_token = {}) const override;
    [[nodiscard]] std::expected<void, TwitchError>
    revoke_access_token(const support::SecretValue& access_token,
                        const std::stop_token& stop_token = {}) const override;
    [[nodiscard]] std::expected<TwitchChatResult, TwitchError>
    send_chat_message(std::string_view authenticated_user_id, std::string_view message,
                      const support::SecretValue& access_token,
                      const std::stop_token& stop_token = {}) const override;

    [[nodiscard]] std::string client_id() const;

  private:
    struct Configuration {
        mutable std::mutex mutex;
        std::string client_id;
    };

    TwitchClient(const ports::IHttpClient& http_client, std::string client_id) noexcept;
    [[nodiscard]] std::expected<std::string, TwitchError> client_id_snapshot() const;

    const ports::IHttpClient& http_client_;
    std::shared_ptr<Configuration> configuration_;
};

} // namespace manny_uploader::providers
