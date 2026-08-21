#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/donbot_verifier.hpp"
#include "manny_uploader/ports/http_client.hpp"
#include "manny_uploader/support/secret_value.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace manny_uploader::providers {

inline constexpr std::string_view donbot_default_api_base = "https://donbot-api.walmslo.com";
inline constexpr std::size_t max_donbot_api_key_bytes = 512;

using DonBotGuild = ports::DonBotGuild;
using DonBotVerification = ports::DonBotVerification;

struct DonBotUploadSuccess {
    std::optional<std::uint64_t> upload_id;
};

enum class DonBotDisposition : std::uint8_t {
    Retry,
    Failed,
    Cancelled,
};

struct DonBotError {
    DonBotDisposition disposition;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<ports::HttpErrorCode> http_error;
    std::optional<std::uint16_t> http_status;
};

class IDonBotClient {
  public:
    virtual ~IDonBotClient() = default;

    [[nodiscard]] virtual std::expected<DonBotVerification, DonBotError>
    verify(std::string_view api_base_url, const support::SecretValue& gw2_api_key,
           const std::stop_token& stop_token = {}) const = 0;

    [[nodiscard]] virtual std::expected<DonBotUploadSuccess, DonBotError>
    upload(const domain::LogFileIdentity& file, std::string_view api_base_url,
           std::string_view guild_id, const support::SecretValue& gw2_api_key,
           const std::stop_token& stop_token = {}) const = 0;
};

class DonBotClient final : public IDonBotClient {
  public:
    explicit DonBotClient(const ports::IHttpClient& http_client) noexcept;

    [[nodiscard]] std::expected<DonBotVerification, DonBotError>
    verify(std::string_view api_base_url, const support::SecretValue& gw2_api_key,
           const std::stop_token& stop_token = {}) const override;

    [[nodiscard]] std::expected<DonBotUploadSuccess, DonBotError>
    upload(const domain::LogFileIdentity& file, std::string_view api_base_url,
           std::string_view guild_id, const support::SecretValue& gw2_api_key,
           const std::stop_token& stop_token = {}) const override;

  private:
    const ports::IHttpClient& http_client_;
};

} // namespace manny_uploader::providers
