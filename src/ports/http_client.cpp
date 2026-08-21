#include "manny_uploader/ports/http_client.hpp"

#include "manny_uploader/support/secret_value.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace manny_uploader::ports {
namespace {

[[nodiscard]] HttpError invalid_request(std::string message) {
    return HttpError{
        .code = HttpErrorCode::InvalidRequest,
        .message = std::move(message),
        .transport_code = std::nullopt,
        .body_error = std::nullopt,
        .system_error = std::nullopt,
    };
}

[[nodiscard]] char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, [](char lhs, char rhs) {
               return ascii_lower(lhs) == ascii_lower(rhs);
           });
}

[[nodiscard]] bool is_http_token_character(char value) noexcept {
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    return punctuation.contains(value);
}

[[nodiscard]] bool is_valid_header_value(std::string_view value) noexcept {
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte == '\t' || (byte >= 0x20U && byte <= 0x7eU);
    });
}

[[nodiscard]] bool has_valid_port(std::string_view suffix) noexcept {
    if (suffix.empty()) {
        return true;
    }
    if (!suffix.starts_with(':') || suffix.size() == 1) {
        return false;
    }
    std::uint32_t port{};
    for (const auto character : suffix.substr(1)) {
        if (character < '0' || character > '9') {
            return false;
        }
        port = (port * 10U) + static_cast<std::uint32_t>(character - '0');
        if (port > 65'535U) {
            return false;
        }
    }
    return port != 0;
}

[[nodiscard]] bool is_loopback_authority(std::string_view authority) noexcept {
    if (authority.starts_with('[')) {
        const auto end = authority.find(']');
        return end != std::string_view::npos &&
               ascii_case_equal(authority.substr(0, end + 1), "[::1]") &&
               has_valid_port(authority.substr(end + 1));
    }

    const auto colon = authority.rfind(':');
    const auto host = colon == std::string_view::npos ? authority : authority.substr(0, colon);
    const auto port =
        colon == std::string_view::npos ? std::string_view{} : authority.substr(colon);
    return (ascii_case_equal(host, "localhost") || host == "127.0.0.1") && has_valid_port(port);
}

[[nodiscard]] std::expected<void, HttpError> validate_url(std::string_view url,
                                                          HttpTransportPolicy policy) {
    if (url.empty() || url.size() > max_http_url_bytes) {
        return std::unexpected(invalid_request("HTTP URL length is invalid"));
    }
    if (!std::ranges::all_of(url, [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x21U && byte <= 0x7eU;
        })) {
        return std::unexpected(invalid_request("HTTP URL contains invalid characters"));
    }
    if (url.contains('#')) {
        return std::unexpected(invalid_request("HTTP URL fragments are not permitted"));
    }

    const auto separator = url.find("://");
    if (separator == std::string_view::npos) {
        return std::unexpected(invalid_request("HTTP URL must be absolute"));
    }
    const auto scheme = url.substr(0, separator);
    const auto authority_start = separator + 3;
    const auto authority_end = url.find_first_of("/?", authority_start);
    const auto authority = url.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.contains('@')) {
        return std::unexpected(invalid_request("HTTP URL authority is invalid"));
    }

    if (ascii_case_equal(scheme, "https")) {
        return {};
    }
    if (ascii_case_equal(scheme, "http") && policy.allow_plaintext_loopback_for_tests &&
        is_loopback_authority(authority)) {
        return {};
    }
    return std::unexpected(invalid_request("HTTP transport requires HTTPS"));
}

[[nodiscard]] bool is_adapter_owned_header(std::string_view name) noexcept {
    constexpr std::array names{
        std::string_view{"host"},
        std::string_view{"content-length"},
        std::string_view{"transfer-encoding"},
        std::string_view{"connection"},
        std::string_view{"expect"},
    };
    return std::ranges::any_of(
        names, [name](std::string_view known) { return ascii_case_equal(name, known); });
}

[[nodiscard]] bool is_known_method(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::Get:
    case HttpMethod::Post:
    case HttpMethod::Put:
    case HttpMethod::Patch:
    case HttpMethod::Delete:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_duration(std::chrono::milliseconds value,
                                  std::chrono::milliseconds maximum) noexcept {
    return value.count() > 0 && value <= maximum;
}

[[nodiscard]] std::expected<void, HttpError>
validate_headers(const std::vector<HttpHeader>& headers) {
    if (headers.size() > max_http_header_count) {
        return std::unexpected(invalid_request("HTTP request has too many headers"));
    }

    std::size_t total_header_bytes{};
    for (const auto& header : headers) {
        if (header.name.empty() || header.name.size() > max_http_header_name_bytes ||
            !std::ranges::all_of(header.name, is_http_token_character)) {
            return std::unexpected(invalid_request("HTTP header name is invalid"));
        }
        if (header.value.size() > max_http_header_value_bytes ||
            !is_valid_header_value(header.value)) {
            return std::unexpected(invalid_request("HTTP header value is invalid"));
        }
        if (is_adapter_owned_header(header.name)) {
            return std::unexpected(
                invalid_request("HTTP framing header is owned by the transport adapter"));
        }
        if (is_sensitive_http_header_name(header.name) &&
            header.sensitivity != HttpHeaderSensitivity::Sensitive) {
            return std::unexpected(
                invalid_request("Sensitive HTTP header must be explicitly marked"));
        }

        const auto encoded_bytes = header.name.size() + header.value.size() + 4U;
        if (encoded_bytes > max_http_request_header_bytes - total_header_bytes) {
            return std::unexpected(invalid_request("HTTP request headers exceed the size limit"));
        }
        total_header_bytes += encoded_bytes;
    }
    return {};
}

[[nodiscard]] std::expected<void, HttpError> validate_body(const HttpRequest& request) {
    if (!request.body) {
        return {};
    }

    const auto length = request.body->content_length();
    if (length == 0 || length > max_http_request_body_bytes) {
        return std::unexpected(invalid_request("HTTP request body length is invalid"));
    }
    if (request.method == HttpMethod::Get) {
        return std::unexpected(invalid_request("HTTP GET request must not have a body"));
    }
    return {};
}

[[nodiscard]] std::expected<void, HttpError>
validate_timeouts_and_limits(const HttpRequest& request) {
    if (!valid_duration(request.timeouts.connect, std::chrono::seconds{60}) ||
        !valid_duration(request.timeouts.operation, std::chrono::minutes{15}) ||
        !valid_duration(request.timeouts.stalled_transfer, std::chrono::minutes{15}) ||
        request.timeouts.stalled_transfer < std::chrono::seconds{1}) {
        return std::unexpected(invalid_request("HTTP timeout configuration is invalid"));
    }
    if (request.response_limits.max_header_bytes == 0 ||
        request.response_limits.max_header_bytes > max_http_response_header_bytes ||
        request.response_limits.max_body_bytes == 0 ||
        request.response_limits.max_body_bytes > max_http_response_body_bytes) {
        return std::unexpected(invalid_request("HTTP response limits are invalid"));
    }
    return {};
}

} // namespace

HttpHeader::~HttpHeader() {
    if (sensitivity == HttpHeaderSensitivity::Sensitive) {
        support::secure_erase(std::as_writable_bytes(std::span{value}));
    }
}

std::string_view http_method_name(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Patch:
        return "PATCH";
    case HttpMethod::Delete:
        return "DELETE";
    }
    return {};
}

bool is_sensitive_http_header_name(std::string_view name) noexcept {
    constexpr std::array names{
        std::string_view{"authorization"}, std::string_view{"proxy-authorization"},
        std::string_view{"cookie"},        std::string_view{"set-cookie"},
        std::string_view{"x-api-key"},     std::string_view{"x-gw2-api-key"},
    };
    return std::ranges::any_of(
        names, [name](std::string_view known) { return ascii_case_equal(name, known); });
}

std::string redacted_http_header_value(const HttpHeader& header) {
    if (header.sensitivity == HttpHeaderSensitivity::Sensitive ||
        is_sensitive_http_header_name(header.name)) {
        return "[REDACTED]";
    }
    return header.value;
}

std::expected<void, HttpError> validate_http_request(const HttpRequest& request,
                                                     HttpTransportPolicy policy) {
    if (!is_known_method(request.method)) {
        return std::unexpected(invalid_request("HTTP method is invalid"));
    }
    auto valid_url = validate_url(request.url, policy);
    if (!valid_url) {
        return valid_url;
    }
    auto valid_headers = validate_headers(request.headers);
    if (!valid_headers) {
        return valid_headers;
    }
    auto valid_body = validate_body(request);
    if (!valid_body) {
        return valid_body;
    }
    return validate_timeouts_and_limits(request);
}

} // namespace manny_uploader::ports
