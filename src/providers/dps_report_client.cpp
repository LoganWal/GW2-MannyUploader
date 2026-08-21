#include "manny_uploader/providers/dps_report_client.hpp"

#include "manny_uploader/http/body_sources.hpp"
#include "manny_uploader/http/multipart_form_data.hpp"
#include "manny_uploader/support/utf8.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::providers {
namespace detail {

struct ParsedEncounter {
    std::optional<bool> success;
    std::optional<std::int64_t> bossId;
    std::optional<std::string> boss;
    std::optional<bool> isCm;
    std::optional<bool> isLegendaryCm;
    std::optional<std::int32_t> emboldened;
};

struct ParsedResponse {
    std::optional<std::string> id;
    std::optional<std::string> permalink;
    std::optional<std::string> userToken;
    std::optional<std::string> error;
    std::optional<ParsedEncounter> encounter;
};

} // namespace detail

namespace {

using detail::ParsedEncounter;
using detail::ParsedResponse;

constexpr auto default_retry_delay = std::chrono::seconds{30};
constexpr auto rate_limit_retry_delay = std::chrono::seconds{60};
constexpr auto maximum_retry_after = std::chrono::seconds{900};

struct ResponseReadOptions : glz::opts {
    bool error_on_unknown_keys{false};
    bool validate_trailing_whitespace{true};
    bool validate_skipped{true};
};

[[nodiscard]] DpsReportUploadError
make_error(DpsReportUploadDisposition disposition, std::string detail,
           std::optional<std::chrono::seconds> retry_after = std::nullopt,
           std::optional<ports::HttpErrorCode> http_error = std::nullopt,
           std::optional<std::uint16_t> http_status = std::nullopt) {
    return DpsReportUploadError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = http_error,
        .http_status = http_status,
    };
}

[[nodiscard]] bool visible_ascii_token(std::span<const std::byte> value) noexcept {
    return value.size() <= max_dps_report_user_token_bytes &&
           std::ranges::all_of(value, [](std::byte byte) {
               const auto character = std::to_integer<unsigned char>(byte);
               return character >= 0x21U && character <= 0x7eU;
           });
}

[[nodiscard]] bool visible_ascii_token(std::string_view value) noexcept {
    return visible_ascii_token(std::as_bytes(std::span{value.data(), value.size()}));
}

[[nodiscard]] bool safe_encounter_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 256 && support::is_valid_utf8(value) &&
           std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20U || byte == 0x7fU;
           });
}

[[nodiscard]] bool valid_permalink(std::string_view value) noexcept {
    if (value.empty() || value.size() > 2048 || !value.starts_with("https://") ||
        value.contains('@') || value.contains('#') || value.contains('?') ||
        !std::ranges::all_of(value, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x21U && byte <= 0x7eU;
        })) {
        return false;
    }

    constexpr std::size_t scheme_size = 8;
    const auto path_start = value.find('/', scheme_size);
    if (path_start == std::string_view::npos || path_start + 1 >= value.size()) {
        return false;
    }
    const auto authority = value.substr(scheme_size, path_start - scheme_size);
    return authority == "dps.report" || authority == "b.dps.report";
}

[[nodiscard]] std::string encounter_mode(const ParsedEncounter& encounter) {
    if (encounter.isLegendaryCm.value_or(false)) {
        return "LCM";
    }
    if (encounter.isCm.value_or(false)) {
        return "CM";
    }
    const auto emboldened = encounter.emboldened.value_or(0);
    if (emboldened > 0) {
        return "Emboldened " + std::to_string(emboldened);
    }
    return {};
}

[[nodiscard]] char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, [](char lhs, char rhs) {
               return ascii_lower(lhs) == ascii_lower(rhs);
           });
}

[[nodiscard]] std::optional<std::chrono::seconds>
numeric_retry_after(const ports::HttpResponse& response) noexcept {
    std::optional<std::string_view> value;
    for (const auto& header : response.headers) {
        if (!ascii_case_equal(header.name, "Retry-After")) {
            continue;
        }
        if (value) {
            return std::nullopt;
        }
        value = header.value;
    }
    if (!value || value->empty()) {
        return std::nullopt;
    }

    std::uint32_t seconds{};
    const auto parsed = std::from_chars(value->data(), value->data() + value->size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || seconds == 0 ||
        seconds > static_cast<std::uint32_t>(maximum_retry_after.count())) {
        return std::nullopt;
    }
    return std::chrono::seconds{seconds};
}

[[nodiscard]] DpsReportUploadError classify_transport_error(const ports::HttpError& error) {
    using ports::HttpErrorCode;
    if (error.code == HttpErrorCode::Cancelled) {
        return make_error(DpsReportUploadDisposition::Cancelled,
                          "The dps.report upload was cancelled", std::nullopt, error.code);
    }

    switch (error.code) {
    case HttpErrorCode::Timeout:
    case HttpErrorCode::NameResolutionFailed:
    case HttpErrorCode::ConnectionFailed:
    case HttpErrorCode::TlsFailed:
    case HttpErrorCode::SendFailed:
    case HttpErrorCode::ReceiveFailed:
        return make_error(DpsReportUploadDisposition::Retry, "dps.report could not be reached",
                          default_retry_delay, error.code);
    case HttpErrorCode::InvalidRequest:
    case HttpErrorCode::BodyReadFailed:
    case HttpErrorCode::ResponseTooLarge:
    case HttpErrorCode::ProtocolError:
    case HttpErrorCode::UnsupportedEnvironment:
    case HttpErrorCode::InitializationFailed:
    case HttpErrorCode::Internal:
        return make_error(DpsReportUploadDisposition::Failed,
                          "The dps.report request could not be completed", std::nullopt,
                          error.code);
    case HttpErrorCode::Cancelled:
        break;
    }
    return make_error(DpsReportUploadDisposition::Failed,
                      "The dps.report request could not be completed", std::nullopt, error.code);
}

[[nodiscard]] DpsReportUploadError classify_status(const ports::HttpResponse& response) {
    const auto status = response.status_code;
    if (status == 408) {
        return make_error(DpsReportUploadDisposition::Retry,
                          "dps.report timed out while processing the upload", default_retry_delay,
                          std::nullopt, status);
    }
    if (status == 429) {
        const auto delay = numeric_retry_after(response).value_or(rate_limit_retry_delay);
        return make_error(DpsReportUploadDisposition::Retry, "dps.report rate limited the upload",
                          delay, std::nullopt, status);
    }
    if (status >= 500) {
        return make_error(DpsReportUploadDisposition::Retry,
                          "dps.report is temporarily unavailable", default_retry_delay,
                          std::nullopt, status);
    }
    return make_error(DpsReportUploadDisposition::Failed, "dps.report rejected the upload",
                      std::nullopt, std::nullopt, status);
}

[[nodiscard]] std::expected<DpsReportUploadSuccess, DpsReportUploadError>
decode_success(const ports::HttpResponse& response, const support::SecretValue* current_token) {
    const auto document =
        std::string_view{reinterpret_cast<const char*>(response.body.data()), response.body.size()};
    ParsedResponse parsed;
    if (const auto parse_error = glz::read<ResponseReadOptions{}>(parsed, document); parse_error) {
        return std::unexpected(make_error(DpsReportUploadDisposition::Failed,
                                          "dps.report returned invalid JSON", std::nullopt,
                                          std::nullopt, response.status_code));
    }
    if (!parsed.permalink || !valid_permalink(*parsed.permalink) || !parsed.encounter ||
        !parsed.encounter->success || !parsed.encounter->bossId || !parsed.encounter->boss ||
        *parsed.encounter->bossId < 0 ||
        std::cmp_greater(*parsed.encounter->bossId, std::numeric_limits<std::uint16_t>::max()) ||
        !safe_encounter_name(*parsed.encounter->boss)) {
        return std::unexpected(make_error(DpsReportUploadDisposition::Failed,
                                          "dps.report returned an incomplete response",
                                          std::nullopt, std::nullopt, response.status_code));
    }

    std::optional<support::SecretValue> replacement_token;
    std::optional<std::string> warning;
    if (parsed.error && !parsed.error->empty()) {
        warning = "dps.report generated the report with a warning";
    }
    if (parsed.userToken && !parsed.userToken->empty()) {
        if (!visible_ascii_token(*parsed.userToken)) {
            warning = "dps.report returned an unusable user token";
        } else {
            auto returned = support::SecretValue::from_text(*parsed.userToken);
            if (current_token == nullptr || returned != *current_token) {
                replacement_token = std::move(returned);
            }
        }
    }

    return DpsReportUploadSuccess{
        .report =
            domain::DpsReportResult{
                .permalink = std::move(*parsed.permalink),
                .encounter_name = std::move(*parsed.encounter->boss),
                .boss_id = static_cast<std::uint16_t>(*parsed.encounter->bossId),
                .mode = encounter_mode(*parsed.encounter),
                .success = *parsed.encounter->success,
            },
        .replacement_user_token = std::move(replacement_token),
        .warning = std::move(warning),
    };
}

[[nodiscard]] std::expected<http::MultipartFormData, DpsReportUploadError>
prepare_body(const domain::LogFileIdentity& file, const support::SecretValue* user_token) {
    if (file.size == 0) {
        return std::unexpected(
            make_error(DpsReportUploadDisposition::Failed, "The dps.report log file is empty"));
    }
    if (user_token != nullptr &&
        (user_token->empty() || !visible_ascii_token(user_token->bytes()))) {
        return std::unexpected(
            make_error(DpsReportUploadDisposition::Failed, "The dps.report user token is invalid"));
    }

    std::vector<http::MultipartPart> parts;
    if (user_token != nullptr) {
        auto token_body = http::make_secret_http_body_source(*user_token);
        if (!token_body) {
            return std::unexpected(make_error(DpsReportUploadDisposition::Failed,
                                              "The dps.report user token could not be prepared"));
        }
        parts.push_back(http::MultipartPart{
            .name = "userToken",
            .filename = std::nullopt,
            .content_type = "text/plain",
            .body = std::move(*token_body),
        });
    }

    auto file_body =
        http::make_file_http_body_source(file.canonical_path, file.size, file.last_write_time);
    if (!file_body) {
        return std::unexpected(make_error(DpsReportUploadDisposition::Failed,
                                          "The dps.report log file is unavailable or changed"));
    }
    parts.push_back(http::MultipartPart{
        .name = "file",
        .filename = "upload.zevtc",
        .content_type = "application/octet-stream",
        .body = std::move(*file_body),
    });

    auto form = http::make_multipart_form_data(std::move(parts));
    if (!form) {
        return std::unexpected(make_error(DpsReportUploadDisposition::Failed,
                                          "The dps.report multipart body could not be prepared"));
    }
    return std::move(*form);
}

} // namespace

DpsReportClient::DpsReportClient(const ports::IHttpClient& http_client) noexcept
    : http_client_{http_client} {}

std::expected<DpsReportUploadSuccess, DpsReportUploadError>
DpsReportClient::upload(const domain::LogFileIdentity& file, const support::SecretValue* user_token,
                        const std::stop_token& stop_token) const {
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(DpsReportUploadDisposition::Cancelled,
                                              "The dps.report upload was cancelled"));
        }
        auto form = prepare_body(file, user_token);
        if (!form) {
            return std::unexpected(std::move(form.error()));
        }

        ports::HttpRequest request;
        request.method = ports::HttpMethod::Post;
        request.url = std::string{dps_report_upload_url};
        request.headers = {
            ports::HttpHeader{
                .name = "Accept",
                .value = "application/json",
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
            ports::HttpHeader{
                .name = "Content-Type",
                .value = std::move(form->content_type),
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
        };
        request.body = std::move(form->body);
        request.timeouts = ports::HttpTimeouts{
            .connect = std::chrono::seconds{10},
            .operation = std::chrono::minutes{15},
            .stalled_transfer = std::chrono::minutes{15},
        };
        request.response_limits = ports::HttpResponseLimits{
            .max_header_bytes = ports::max_http_response_header_bytes,
            .max_body_bytes = std::size_t{1} * 1024U * 1024U,
        };

        auto response = http_client_.execute(std::move(request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error()));
        }
        if (response->status_code < 200 || response->status_code >= 300) {
            return std::unexpected(classify_status(*response));
        }
        return decode_success(*response, user_token);
    } catch (...) {
        return std::unexpected(make_error(DpsReportUploadDisposition::Failed,
                                          "The dps.report client failed unexpectedly"));
    }
}

} // namespace manny_uploader::providers
