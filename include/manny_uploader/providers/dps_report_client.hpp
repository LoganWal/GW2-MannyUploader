#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/http_client.hpp"
#include "manny_uploader/support/secret_value.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace manny_uploader::providers {

inline constexpr std::string_view dps_report_upload_url =
    "https://dps.report/uploadContent?json=1&generator=ei";
inline constexpr std::string_view dps_report_detailed_wvw_upload_url =
    "https://dps.report/uploadContent?json=1&generator=ei&detailedwvw=true";
inline constexpr std::size_t max_dps_report_user_token_bytes = 256;

struct DpsReportUploadOptions {
    bool detailed_wvw{};

    [[nodiscard]] friend bool operator==(DpsReportUploadOptions,
                                         DpsReportUploadOptions) noexcept = default;
};

enum class DpsReportUploadDisposition : std::uint8_t {
    Retry,
    Failed,
    Cancelled,
};

struct DpsReportUploadError {
    DpsReportUploadDisposition disposition;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<ports::HttpErrorCode> http_error;
    std::optional<std::uint16_t> http_status;
};

struct DpsReportUploadSuccess {
    domain::DpsReportResult report;
    std::optional<support::SecretValue> replacement_user_token;
    std::optional<std::string> warning;
};

class IDpsReportClient {
  public:
    virtual ~IDpsReportClient() = default;

    [[nodiscard]] virtual std::expected<DpsReportUploadSuccess, DpsReportUploadError>
    upload(const domain::LogFileIdentity& file, const support::SecretValue* user_token = nullptr,
           const std::stop_token& stop_token = {}, DpsReportUploadOptions options = {}) const = 0;
};

class DpsReportClient final : public IDpsReportClient {
  public:
    explicit DpsReportClient(const ports::IHttpClient& http_client) noexcept;

    [[nodiscard]] std::expected<DpsReportUploadSuccess, DpsReportUploadError>
    upload(const domain::LogFileIdentity& file, const support::SecretValue* user_token = nullptr,
           const std::stop_token& stop_token = {},
           DpsReportUploadOptions options = {}) const override;

  private:
    const ports::IHttpClient& http_client_;
};

} // namespace manny_uploader::providers
