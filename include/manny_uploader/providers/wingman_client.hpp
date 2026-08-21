#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/http_client.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace manny_uploader::providers {

inline constexpr std::string_view wingman_compat_upload_url = "https://evtc.bel.st/evtc";

enum class WingmanUploadDisposition : std::uint8_t {
    Retry,
    Failed,
    Cancelled,
};

struct WingmanUploadError {
    WingmanUploadDisposition disposition;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<ports::HttpErrorCode> http_error;
    std::optional<std::uint16_t> http_status;
};

struct WingmanUploadSuccess {
    bool duplicate{};
};

class IWingmanClient {
  public:
    virtual ~IWingmanClient() = default;

    [[nodiscard]] virtual std::expected<WingmanUploadSuccess, WingmanUploadError>
    upload(const domain::LogFileIdentity& file, const domain::EncounterMetadata& metadata,
           const std::stop_token& stop_token = {}) const = 0;
};

class WingmanClient final : public IWingmanClient {
  public:
    explicit WingmanClient(const ports::IHttpClient& http_client,
                           std::string upload_url = std::string{wingman_compat_upload_url});

    [[nodiscard]] std::expected<WingmanUploadSuccess, WingmanUploadError>
    upload(const domain::LogFileIdentity& file, const domain::EncounterMetadata& metadata,
           const std::stop_token& stop_token = {}) const override;

  private:
    const ports::IHttpClient& http_client_;
    std::string upload_url_;
};

} // namespace manny_uploader::providers
