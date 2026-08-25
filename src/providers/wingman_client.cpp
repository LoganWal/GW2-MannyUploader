#include "manny_uploader/providers/wingman_client.hpp"

#include "manny_uploader/http/body_sources.hpp"
#include "manny_uploader/http/multipart_form_data.hpp"
#include "manny_uploader/support/utf8.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::providers {
namespace detail {

struct ParsedWingmanResponse {
    std::optional<bool> result;
    std::optional<std::uint64_t> ticket;
};

struct ParsedTicketStatus {
    std::optional<std::string> state;
};

struct ParsedWingmanLog {
    std::optional<std::string> html;
};

struct ParsedCheckResponse {
    std::optional<bool> success;
    std::optional<ParsedWingmanLog> log;
};

} // namespace detail

namespace {

constexpr auto default_retry_delay = std::chrono::seconds{30};
constexpr auto rate_limit_retry_delay = std::chrono::seconds{60};
constexpr auto maximum_retry_after = std::chrono::seconds{900};
constexpr std::size_t max_account_bytes = 256;

struct ResponseReadOptions : glz::opts {
    bool error_on_unknown_keys{false};
    bool validate_trailing_whitespace{true};
    bool validate_skipped{true};
};

[[nodiscard]] WingmanUploadError
make_error(WingmanUploadDisposition disposition, std::string detail,
           std::optional<std::chrono::seconds> retry_after = std::nullopt,
           std::optional<ports::HttpErrorCode> http_error = std::nullopt,
           std::optional<std::uint16_t> http_status = std::nullopt) {
    return WingmanUploadError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = http_error,
        .http_status = http_status,
    };
}

[[nodiscard]] std::vector<std::byte> bytes(std::string_view value) {
    const auto characters = std::span{value.data(), value.size()};
    const auto view = std::as_bytes(characters);
    return {view.begin(), view.end()};
}

[[nodiscard]] bool valid_account(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_account_bytes && support::is_valid_utf8(value) &&
           std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20U || byte == 0x7fU;
           });
}

[[nodiscard]] bool valid_upload_url(std::string_view value) noexcept {
    return value.starts_with("https://") && value.size() > 8 && value.size() <= 2048 &&
           !value.contains('@') && !value.contains('#') &&
           std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte <= 0x20U || byte == 0x7fU;
           });
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

[[nodiscard]] WingmanUploadError classify_transport_error(const ports::HttpError& error) {
    using ports::HttpErrorCode;
    if (error.code == HttpErrorCode::Cancelled) {
        return make_error(WingmanUploadDisposition::Cancelled,
                          "The GW2Wingman upload was cancelled", std::nullopt, error.code);
    }

    switch (error.code) {
    case HttpErrorCode::Timeout:
    case HttpErrorCode::NameResolutionFailed:
    case HttpErrorCode::ConnectionFailed:
    case HttpErrorCode::TlsFailed:
    case HttpErrorCode::SendFailed:
    case HttpErrorCode::ReceiveFailed:
        return make_error(WingmanUploadDisposition::Retry,
                          "The GW2Wingman compatibility service could not be reached",
                          default_retry_delay, error.code);
    case HttpErrorCode::InvalidRequest:
    case HttpErrorCode::BodyReadFailed:
    case HttpErrorCode::ResponseTooLarge:
    case HttpErrorCode::ProtocolError:
    case HttpErrorCode::UnsupportedEnvironment:
    case HttpErrorCode::InitializationFailed:
    case HttpErrorCode::Internal:
        return make_error(WingmanUploadDisposition::Failed,
                          "The GW2Wingman request could not be completed", std::nullopt,
                          error.code);
    case HttpErrorCode::Cancelled:
        break;
    }
    return make_error(WingmanUploadDisposition::Failed,
                      "The GW2Wingman request could not be completed", std::nullopt, error.code);
}

[[nodiscard]] WingmanUploadError classify_status(const ports::HttpResponse& response) {
    const auto status = response.status_code;
    if (status == 408) {
        return make_error(WingmanUploadDisposition::Retry,
                          "GW2Wingman timed out while processing the upload", default_retry_delay,
                          std::nullopt, status);
    }
    if (status == 429) {
        const auto delay = numeric_retry_after(response).value_or(rate_limit_retry_delay);
        return make_error(WingmanUploadDisposition::Retry, "GW2Wingman rate limited the upload",
                          delay, std::nullopt, status);
    }
    if (status >= 500) {
        return make_error(WingmanUploadDisposition::Retry,
                          "The GW2Wingman compatibility service is temporarily unavailable",
                          default_retry_delay, std::nullopt, status);
    }
    return make_error(WingmanUploadDisposition::Failed, "GW2Wingman rejected the upload",
                      std::nullopt, std::nullopt, status);
}

struct AcceptedUpload {
    std::optional<std::uint64_t> ticket;
};

[[nodiscard]] std::expected<AcceptedUpload, WingmanUploadError>
decode_success(const ports::HttpResponse& response) {
    const auto document =
        std::string_view{reinterpret_cast<const char*>(response.body.data()), response.body.size()};
    detail::ParsedWingmanResponse parsed;
    if (const auto parse_error = glz::read<ResponseReadOptions{}>(parsed, document); parse_error) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman returned invalid JSON", std::nullopt,
                                          std::nullopt, response.status_code));
    }
    if (!parsed.result.has_value()) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman returned an incomplete response",
                                          std::nullopt, std::nullopt, response.status_code));
    }
    if (!*parsed.result) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman declined the upload", std::nullopt,
                                          std::nullopt, response.status_code));
    }
    if (parsed.ticket && *parsed.ticket == 0) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman returned an invalid processing ticket",
                                          std::nullopt, std::nullopt, response.status_code));
    }
    return AcceptedUpload{.ticket = parsed.ticket};
}

[[nodiscard]] bool safe_wingman_slug(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 512 && std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte >= static_cast<unsigned char>('a') &&
                byte <= static_cast<unsigned char>('z')) ||
               (byte >= static_cast<unsigned char>('A') &&
                byte <= static_cast<unsigned char>('Z')) ||
               (byte >= static_cast<unsigned char>('0') &&
                byte <= static_cast<unsigned char>('9')) ||
               character == '-' || character == '_';
    });
}

[[nodiscard]] std::string form_encode(std::string_view value) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
            (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) ||
            (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) ||
            character == '-' || character == '_' || character == '.' || character == '~') {
            encoded.push_back(character);
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4U]);
            encoded.push_back(hex[byte & 0x0fU]);
        }
    }
    return encoded;
}

[[nodiscard]] bool wait_for_poll(std::chrono::milliseconds delay,
                                 const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return false;
    }
    if (delay <= std::chrono::milliseconds::zero()) {
        return true;
    }
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock{mutex};
    condition.wait_for(lock, stop_token, delay, [] { return false; });
    return !stop_token.stop_requested();
}

[[nodiscard]] ports::HttpRequest json_request(ports::HttpMethod method, std::string url) {
    return ports::HttpRequest{
        .method = method,
        .url = std::move(url),
        .headers = {ports::HttpHeader{
            .name = "Accept",
            .value = "application/json",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        }},
        .body = nullptr,
        .timeouts =
            ports::HttpTimeouts{
                .connect = std::chrono::seconds{10},
                .operation = std::chrono::seconds{60},
                .stalled_transfer = std::chrono::seconds{30},
            },
        .response_limits =
            ports::HttpResponseLimits{
                .max_header_bytes = ports::max_http_response_header_bytes,
                .max_body_bytes = std::size_t{64} * 1024U,
            },
    };
}

[[nodiscard]] std::expected<bool, WingmanUploadError>
ticket_finished(const ports::IHttpClient& http_client, std::uint64_t ticket,
                const std::stop_token& stop_token) {
    auto response = http_client.execute(
        json_request(ports::HttpMethod::Get,
                     std::string{wingman_compat_status_base_url} + std::to_string(ticket)),
        stop_token);
    if (!response) {
        return std::unexpected(classify_transport_error(response.error()));
    }
    if (response->status_code == 404) {
        return true;
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        return std::unexpected(classify_status(*response));
    }
    const auto document = std::string_view{reinterpret_cast<const char*>(response->body.data()),
                                           response->body.size()};
    detail::ParsedTicketStatus parsed;
    if (const auto parse_error = glz::read<ResponseReadOptions{}>(parsed, document);
        parse_error || !parsed.state) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman returned an invalid ticket status"));
    }
    if (*parsed.state == "uploaded" || *parsed.state == "skipped") {
        return true;
    }
    if (*parsed.state == "failed") {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman could not process the upload"));
    }
    if (*parsed.state == "queued" || *parsed.state == "processing" || *parsed.state == "deferred") {
        return false;
    }
    return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                      "GW2Wingman returned an unknown ticket status"));
}

[[nodiscard]] std::expected<std::optional<std::string>, WingmanUploadError>
find_permalink(const ports::IHttpClient& http_client, const domain::LogFileIdentity& file,
               const domain::EncounterMetadata& metadata, const std::stop_token& stop_token) {
    std::string remote_name = metadata.pov_account;
    std::erase(remote_name, '.');
    remote_name += "_upload.zevtc";
    const auto body_text = "file=" + form_encode(remote_name) +
                           "&filesize=" + form_encode(std::to_string(file.size)) +
                           "&bossID=" + form_encode(std::to_string(metadata.boss_id)) +
                           "&account=" + form_encode(metadata.pov_account);
    auto request = json_request(ports::HttpMethod::Post, std::string{wingman_check_upload_url});
    request.headers.push_back(ports::HttpHeader{
        .name = "Content-Type",
        .value = "application/x-www-form-urlencoded",
        .sensitivity = ports::HttpHeaderSensitivity::Public,
    });
    const auto body_bytes = std::as_bytes(std::span{body_text.data(), body_text.size()});
    request.body = http::make_memory_http_body_source(
        std::vector<std::byte>{body_bytes.begin(), body_bytes.end()});
    auto response = http_client.execute(std::move(request), stop_token);
    if (!response) {
        return std::unexpected(classify_transport_error(response.error()));
    }
    if (response->status_code < 200 || response->status_code >= 300) {
        return std::unexpected(classify_status(*response));
    }
    const auto document = std::string_view{reinterpret_cast<const char*>(response->body.data()),
                                           response->body.size()};
    detail::ParsedCheckResponse parsed;
    if (const auto parse_error = glz::read<ResponseReadOptions{}>(parsed, document);
        parse_error || !parsed.success) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman returned an invalid permalink response"));
    }
    if (!*parsed.success) {
        return std::optional<std::string>{};
    }
    if (!parsed.log || !parsed.log->html || !safe_wingman_slug(*parsed.log->html)) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "GW2Wingman returned an invalid permalink"));
    }
    return std::string{wingman_log_base_url} + *parsed.log->html;
}

[[nodiscard]] std::expected<http::MultipartFormData, WingmanUploadError>
prepare_body(const domain::LogFileIdentity& file, const domain::EncounterMetadata& metadata) {
    if (file.size == 0) {
        return std::unexpected(
            make_error(WingmanUploadDisposition::Failed, "The GW2Wingman log file is empty"));
    }
    if (!valid_account(metadata.pov_account)) {
        return std::unexpected(
            make_error(WingmanUploadDisposition::Failed, "The GW2Wingman POV account is invalid"));
    }
    if (metadata.boss_id == 0) {
        return std::unexpected(
            make_error(WingmanUploadDisposition::Failed, "The GW2Wingman encounter ID is invalid"));
    }

    const auto text_part = [](std::string_view name, std::string_view value) {
        return http::MultipartPart{
            .name = std::string{name},
            .filename = std::nullopt,
            .content_type = "text/plain",
            .body = http::make_memory_http_body_source(bytes(value)),
        };
    };

    std::vector<http::MultipartPart> parts;
    parts.reserve(4);
    parts.push_back(text_part("account", metadata.pov_account));
    parts.push_back(text_part("filesize", std::to_string(file.size)));
    parts.push_back(text_part("triggerID", std::to_string(metadata.boss_id)));

    auto file_body =
        http::make_file_http_body_source(file.canonical_path, file.size, file.last_write_time);
    if (!file_body) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "The GW2Wingman log file is unavailable or changed"));
    }
    parts.push_back(http::MultipartPart{
        .name = "file",
        .filename = "upload.zevtc",
        .content_type = "application/octet-stream",
        .body = std::move(*file_body),
    });

    auto form = http::make_multipart_form_data(std::move(parts));
    if (!form) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "The GW2Wingman multipart body could not be prepared"));
    }
    return std::move(*form);
}

} // namespace

WingmanClient::WingmanClient(const ports::IHttpClient& http_client, std::string upload_url,
                             WingmanPollingOptions polling)
    : http_client_{http_client}, upload_url_{std::move(upload_url)}, polling_{polling} {}

std::expected<WingmanUploadSuccess, WingmanUploadError>
WingmanClient::upload(const domain::LogFileIdentity& file,
                      const domain::EncounterMetadata& metadata,
                      const std::stop_token& stop_token) const {
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(WingmanUploadDisposition::Cancelled,
                                              "The GW2Wingman upload was cancelled"));
        }
        if (!valid_upload_url(upload_url_)) {
            return std::unexpected(
                make_error(WingmanUploadDisposition::Failed, "The GW2Wingman endpoint is invalid"));
        }
        auto form = prepare_body(file, metadata);
        if (!form) {
            return std::unexpected(std::move(form.error()));
        }

        ports::HttpRequest request;
        request.method = ports::HttpMethod::Post;
        request.url = upload_url_;
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
            .max_body_bytes = std::size_t{64} * 1024U,
        };

        auto response = http_client_.execute(std::move(request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error()));
        }
        const bool duplicate = response->status_code == 409;
        std::optional<std::uint64_t> ticket;
        if (!duplicate) {
            if (response->status_code < 200 || response->status_code >= 300) {
                return std::unexpected(classify_status(*response));
            }
            auto accepted = decode_success(*response);
            if (!accepted) {
                return std::unexpected(std::move(accepted.error()));
            }
            ticket = accepted->ticket;
            if (!ticket) {
                return WingmanUploadSuccess{.duplicate = false, .permalink = std::nullopt};
            }
        }

        if (ticket) {
            const auto deadline = std::chrono::steady_clock::now() + polling_.ticket_timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!wait_for_poll(polling_.ticket_interval, stop_token)) {
                    return std::unexpected(make_error(WingmanUploadDisposition::Cancelled,
                                                      "The GW2Wingman upload was cancelled"));
                }
                auto finished = ticket_finished(http_client_, *ticket, stop_token);
                if (!finished) {
                    return std::unexpected(std::move(finished.error()));
                }
                if (*finished) {
                    break;
                }
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + polling_.check_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto permalink = find_permalink(http_client_, file, metadata, stop_token);
            if (!permalink) {
                return std::unexpected(std::move(permalink.error()));
            }
            if (*permalink) {
                return WingmanUploadSuccess{.duplicate = duplicate,
                                            .permalink = std::move(**permalink)};
            }
            if (!wait_for_poll(polling_.check_interval, stop_token)) {
                return std::unexpected(make_error(WingmanUploadDisposition::Cancelled,
                                                  "The GW2Wingman upload was cancelled"));
            }
        }
        return WingmanUploadSuccess{.duplicate = duplicate, .permalink = std::nullopt};
    } catch (...) {
        return std::unexpected(make_error(WingmanUploadDisposition::Failed,
                                          "The GW2Wingman client failed unexpectedly"));
    }
}

} // namespace manny_uploader::providers
