#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manny_uploader::ports {

inline constexpr std::size_t max_http_url_bytes = 4U * 1024U;
inline constexpr std::size_t max_http_header_count = 64;
inline constexpr std::size_t max_http_header_name_bytes = 128;
inline constexpr std::size_t max_http_header_value_bytes = 8U * 1024U;
inline constexpr std::size_t max_http_request_header_bytes = 32U * 1024U;
inline constexpr std::uint64_t max_http_request_body_bytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t max_http_response_header_bytes = 64U * 1024U;
inline constexpr std::size_t max_http_response_body_bytes = 16U * 1024U * 1024U;

enum class HttpMethod : std::uint8_t {
    Get,
    Post,
    Put,
    Patch,
    Delete,
};

[[nodiscard]] std::string_view http_method_name(HttpMethod method) noexcept;

enum class HttpHeaderSensitivity : std::uint8_t {
    Public,
    Sensitive,
};

struct HttpHeader {
    std::string name;
    std::string value;
    HttpHeaderSensitivity sensitivity{HttpHeaderSensitivity::Public};

    ~HttpHeader();
};

[[nodiscard]] bool is_sensitive_http_header_name(std::string_view name) noexcept;
[[nodiscard]] std::string redacted_http_header_value(const HttpHeader& header);

enum class HttpBodyReadErrorCode : std::uint8_t {
    Cancelled,
    SourceUnavailable,
    SourceChanged,
    ReadFailed,
};

struct HttpBodyReadError {
    HttpBodyReadErrorCode code;
    std::string message;
    std::optional<std::uint32_t> system_error;
};

class IHttpBodySource {
  public:
    virtual ~IHttpBodySource() = default;

    [[nodiscard]] virtual std::uint64_t content_length() const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::size_t, HttpBodyReadError>
    read(std::span<std::byte> destination, const std::stop_token& stop_token) = 0;
};

struct HttpTimeouts {
    std::chrono::milliseconds connect{std::chrono::seconds{10}};
    std::chrono::milliseconds operation{std::chrono::minutes{2}};
    std::chrono::milliseconds stalled_transfer{std::chrono::seconds{30}};
};

struct HttpResponseLimits {
    std::size_t max_header_bytes{max_http_response_header_bytes};
    std::size_t max_body_bytes{1U * 1024U * 1024U};
};

struct HttpRequest {
    HttpMethod method{HttpMethod::Get};
    std::string url;
    std::vector<HttpHeader> headers;
    std::unique_ptr<IHttpBodySource> body;
    HttpTimeouts timeouts;
    HttpResponseLimits response_limits;
};

struct HttpResponse {
    std::uint16_t status_code;
    std::vector<HttpHeader> headers;
    std::vector<std::byte> body;
};

enum class HttpErrorCode : std::uint8_t {
    InvalidRequest,
    BodyReadFailed,
    Cancelled,
    Timeout,
    NameResolutionFailed,
    ConnectionFailed,
    TlsFailed,
    SendFailed,
    ReceiveFailed,
    ResponseTooLarge,
    ProtocolError,
    UnsupportedEnvironment,
    InitializationFailed,
    Internal,
};

struct HttpError {
    HttpErrorCode code;
    std::string message;
    std::optional<std::int64_t> transport_code;
    std::optional<HttpBodyReadErrorCode> body_error;
    std::optional<std::uint32_t> system_error;
};

struct HttpTransportPolicy {
    bool allow_plaintext_loopback_for_tests{};
};

[[nodiscard]] std::expected<void, HttpError> validate_http_request(const HttpRequest& request,
                                                                   HttpTransportPolicy policy = {});

class IHttpClient {
  public:
    virtual ~IHttpClient() = default;

    [[nodiscard]] virtual std::expected<HttpResponse, HttpError>
    execute(HttpRequest request, const std::stop_token& stop_token = {}) const = 0;
};

} // namespace manny_uploader::ports
