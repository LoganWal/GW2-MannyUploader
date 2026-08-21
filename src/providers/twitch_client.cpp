#include "manny_uploader/providers/twitch_client.hpp"

#include "manny_uploader/http/body_sources.hpp"
#include "manny_uploader/support/utf8.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manny_uploader::providers {
namespace detail {

struct ParsedTwitchDeviceAuthorization {
    std::optional<std::string> device_code;
    std::optional<std::int64_t> expires_in;
    std::optional<std::int64_t> interval;
    std::optional<std::string> user_code;
    std::optional<std::string> verification_uri;
};

using ParsedTwitchScope = std::variant<std::string, std::vector<std::string>>;

struct ParsedTwitchTokenGrant {
    std::optional<std::string> access_token;
    std::optional<std::int64_t> expires_in;
    std::optional<std::string> refresh_token;
    std::optional<ParsedTwitchScope> scope;
    std::optional<std::string> token_type;
};

struct ParsedTwitchError {
    std::optional<std::int64_t> status;
    std::optional<std::string> message;
};

struct ParsedTwitchValidation {
    std::optional<std::string> client_id;
    std::optional<std::string> login;
    std::optional<std::vector<std::string>> scopes;
    std::optional<std::string> user_id;
    std::optional<std::int64_t> expires_in;
};

struct ParsedTwitchDropReason {
    std::optional<std::string> code;
    std::optional<std::string> message;
};

struct ParsedTwitchChatResult {
    std::optional<std::string> message_id;
    std::optional<bool> is_sent;
    std::optional<ParsedTwitchDropReason> drop_reason;
};

struct ParsedTwitchChatResponse {
    std::optional<std::vector<ParsedTwitchChatResult>> data;
};

struct TwitchChatRequestBody {
    std::string broadcaster_id;
    std::string sender_id;
    std::string message;
};

} // namespace detail

namespace {

using namespace std::chrono_literals;

constexpr std::string_view device_url = "https://id.twitch.tv/oauth2/device";
constexpr std::string_view token_url = "https://id.twitch.tv/oauth2/token";
constexpr std::string_view validate_url = "https://id.twitch.tv/oauth2/validate";
constexpr std::string_view revoke_url = "https://id.twitch.tv/oauth2/revoke";
constexpr std::string_view chat_url = "https://api.twitch.tv/helix/chat/messages";
constexpr std::string_view device_grant_type = "urn:ietf:params:oauth:grant-type:device_code";
constexpr std::size_t max_oauth_response_bytes = std::size_t{64} * 1024U;
constexpr std::size_t max_chat_response_bytes = std::size_t{64} * 1024U;
constexpr auto default_retry_delay = 30s;
constexpr auto rate_limit_retry_delay = 60s;
constexpr auto maximum_retry_after = 15min;

struct ResponseReadOptions : glz::opts {
    bool error_on_unknown_keys{false};
    bool validate_trailing_whitespace{true};
    bool validate_skipped{true};
};

struct RequestWriteOptions : glz::opts {
    bool escape_control_characters{true};
};

enum class TwitchOperation : std::uint8_t {
    StartDeviceAuthorization,
    PollDeviceAuthorization,
    ValidateAccessToken,
    RefreshAccessToken,
    RevokeAccessToken,
    SendChatMessage,
};

class ResponseBodyWiper {
  public:
    explicit ResponseBodyWiper(std::vector<std::byte>& body) noexcept : body_{body} {}
    ~ResponseBodyWiper() {
        support::secure_erase(body_);
    }

    ResponseBodyWiper(const ResponseBodyWiper&) = delete;
    ResponseBodyWiper& operator=(const ResponseBodyWiper&) = delete;

  private:
    std::vector<std::byte>& body_;
};

void wipe_string(std::string& value) noexcept {
    support::secure_erase(std::as_writable_bytes(std::span{value.data(), value.size()}));
    value.clear();
}

class DeviceResponseWiper {
  public:
    explicit DeviceResponseWiper(detail::ParsedTwitchDeviceAuthorization& response) noexcept
        : response_{response} {}
    ~DeviceResponseWiper() {
        if (response_.device_code) {
            wipe_string(*response_.device_code);
        }
    }

    DeviceResponseWiper(const DeviceResponseWiper&) = delete;
    DeviceResponseWiper& operator=(const DeviceResponseWiper&) = delete;

  private:
    detail::ParsedTwitchDeviceAuthorization& response_;
};

class TokenResponseWiper {
  public:
    explicit TokenResponseWiper(detail::ParsedTwitchTokenGrant& response) noexcept
        : response_{response} {}
    ~TokenResponseWiper() {
        if (response_.access_token) {
            wipe_string(*response_.access_token);
        }
        if (response_.refresh_token) {
            wipe_string(*response_.refresh_token);
        }
    }

    TokenResponseWiper(const TokenResponseWiper&) = delete;
    TokenResponseWiper& operator=(const TokenResponseWiper&) = delete;

  private:
    detail::ParsedTwitchTokenGrant& response_;
};

class StringWiper {
  public:
    explicit StringWiper(std::string& value) noexcept : value_{value} {}
    ~StringWiper() {
        wipe_string(value_);
    }

    StringWiper(const StringWiper&) = delete;
    StringWiper& operator=(const StringWiper&) = delete;

  private:
    std::string& value_;
};

[[nodiscard]] TwitchError make_error(TwitchDisposition disposition, std::string detail,
                                     std::optional<std::chrono::seconds> retry_after = std::nullopt,
                                     std::optional<ports::HttpErrorCode> http_error = std::nullopt,
                                     std::optional<std::uint16_t> http_status = std::nullopt) {
    return TwitchError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = http_error,
        .http_status = http_status,
    };
}

[[nodiscard]] std::string_view operation_name(TwitchOperation operation) noexcept {
    switch (operation) {
    case TwitchOperation::StartDeviceAuthorization:
        return "Twitch authorization";
    case TwitchOperation::PollDeviceAuthorization:
        return "Twitch authorization polling";
    case TwitchOperation::ValidateAccessToken:
        return "Twitch session validation";
    case TwitchOperation::RefreshAccessToken:
        return "Twitch session refresh";
    case TwitchOperation::RevokeAccessToken:
        return "Twitch disconnect";
    case TwitchOperation::SendChatMessage:
        return "Twitch chat delivery";
    }
    return "Twitch request";
}

[[nodiscard]] bool valid_client_id(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    });
}

[[nodiscard]] bool valid_secret(const support::SecretValue& value) noexcept {
    return !value.empty() && value.size() <= max_twitch_token_bytes &&
           std::ranges::all_of(value.bytes(), [](std::byte byte) {
               const auto character = std::to_integer<unsigned char>(byte);
               return character >= 0x21U && character <= 0x7eU;
           });
}

[[nodiscard]] bool valid_secret(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_twitch_token_bytes &&
           std::ranges::all_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte >= 0x21U && byte <= 0x7eU;
           });
}

[[nodiscard]] bool safe_visible_text(std::string_view value, std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes && support::is_valid_utf8(value) &&
           std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20U || byte == 0x7fU;
           });
}

[[nodiscard]] bool valid_user_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 20 || value.front() == '0') {
        return false;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed > 0;
}

[[nodiscard]] bool valid_login(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_';
    });
}

[[nodiscard]] bool valid_verification_uri(std::string_view value) noexcept {
    constexpr std::string_view base = "https://www.twitch.tv/activate";
    return value.size() <= 2048 && value.starts_with(base) &&
           (value.size() == base.size() || value[base.size()] == '?') && !value.contains('@') &&
           !value.contains('#') && !value.contains('\\') &&
           std::ranges::all_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte >= 0x21U && byte <= 0x7eU;
           });
}

[[nodiscard]] bool valid_duration(std::int64_t value) noexcept {
    return value > 0 && value <= std::chrono::duration_cast<std::chrono::seconds>(24h * 7).count();
}

[[nodiscard]] bool valid_scope(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == ':' || character == '_';
    });
}

[[nodiscard]] bool valid_scopes(const std::vector<std::string>& scopes) noexcept {
    return scopes.size() == 1 && scopes.front() == twitch_chat_scope &&
           std::ranges::all_of(scopes, valid_scope);
}

[[nodiscard]] std::vector<std::string> take_scopes(detail::ParsedTwitchScope& parsed_scope) {
    if (auto* single = std::get_if<std::string>(&parsed_scope)) {
        std::vector<std::string> result;
        result.push_back(std::move(*single));
        return result;
    }
    return std::move(std::get<std::vector<std::string>>(parsed_scope));
}

[[nodiscard]] bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, [](char lhs, char rhs) {
               const auto lower = [](char value) {
                   return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                                       : value;
               };
               return lower(lhs) == lower(rhs);
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

[[nodiscard]] TwitchError classify_transport_error(const ports::HttpError& error,
                                                   TwitchOperation operation) {
    const auto name = std::string{operation_name(operation)};
    if (error.code == ports::HttpErrorCode::Cancelled) {
        return make_error(TwitchDisposition::Cancelled, name + " was cancelled", std::nullopt,
                          error.code);
    }

    switch (error.code) {
    case ports::HttpErrorCode::Timeout:
    case ports::HttpErrorCode::NameResolutionFailed:
    case ports::HttpErrorCode::ConnectionFailed:
    case ports::HttpErrorCode::TlsFailed:
    case ports::HttpErrorCode::SendFailed:
    case ports::HttpErrorCode::ReceiveFailed:
        return make_error(TwitchDisposition::Retry, name + " could not reach Twitch",
                          default_retry_delay, error.code);
    case ports::HttpErrorCode::InvalidRequest:
    case ports::HttpErrorCode::BodyReadFailed:
    case ports::HttpErrorCode::ResponseTooLarge:
    case ports::HttpErrorCode::ProtocolError:
    case ports::HttpErrorCode::UnsupportedEnvironment:
    case ports::HttpErrorCode::InitializationFailed:
    case ports::HttpErrorCode::Internal:
        return make_error(TwitchDisposition::Failed, name + " could not be completed", std::nullopt,
                          error.code);
    case ports::HttpErrorCode::Cancelled:
        break;
    }
    return make_error(TwitchDisposition::Failed, name + " could not be completed", std::nullopt,
                      error.code);
}

[[nodiscard]] bool reconnect_status(TwitchOperation operation, std::uint16_t status) noexcept {
    if (operation == TwitchOperation::RefreshAccessToken) {
        return status == 400 || status == 401;
    }
    return (operation == TwitchOperation::ValidateAccessToken ||
            operation == TwitchOperation::SendChatMessage) &&
           status == 401;
}

[[nodiscard]] TwitchError classify_status(const ports::HttpResponse& response,
                                          TwitchOperation operation) {
    const auto status = response.status_code;
    const auto name = std::string{operation_name(operation)};
    if (reconnect_status(operation, status)) {
        return make_error(TwitchDisposition::Reconnect, name + " requires reconnection",
                          std::nullopt, std::nullopt, status);
    }
    if (status == 408) {
        return make_error(TwitchDisposition::Retry, name + " timed out", default_retry_delay,
                          std::nullopt, status);
    }
    if (status == 429) {
        return make_error(TwitchDisposition::Retry, name + " was rate limited",
                          numeric_retry_after(response).value_or(rate_limit_retry_delay),
                          std::nullopt, status);
    }
    if (status >= 500) {
        return make_error(TwitchDisposition::Retry, "Twitch is temporarily unavailable",
                          default_retry_delay, std::nullopt, status);
    }
    return make_error(TwitchDisposition::Failed, name + " was rejected", std::nullopt, std::nullopt,
                      status);
}

void append_percent_encoded(std::string& destination, std::string_view value) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const auto unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~';
        if (unreserved) {
            destination.push_back(character);
            continue;
        }
        destination.push_back('%');
        destination.push_back(hex[(byte >> 4U) & 0x0fU]);
        destination.push_back(hex[byte & 0x0fU]);
    }
}

void append_percent_encoded(std::string& destination, const support::SecretValue& value) {
    const auto text =
        std::string_view{reinterpret_cast<const char*>(value.bytes().data()), value.bytes().size()};
    append_percent_encoded(destination, text);
}

struct FormFieldName {
    std::string_view value;
};

void append_form_field(std::string& form, FormFieldName name, std::string_view value) {
    if (!form.empty()) {
        form.push_back('&');
    }
    form.append(name.value);
    form.push_back('=');
    append_percent_encoded(form, value);
}

void append_form_field(std::string& form, FormFieldName name, const support::SecretValue& value) {
    if (!form.empty()) {
        form.push_back('&');
    }
    form.append(name.value);
    form.push_back('=');
    append_percent_encoded(form, value);
}

[[nodiscard]] std::expected<std::unique_ptr<ports::IHttpBodySource>, TwitchError>
make_form_body(std::string& form, TwitchOperation operation) {
    StringWiper wiper{form};
    auto form_wiper = support::SecretValue::from_text(form);
    auto body = http::make_secret_http_body_source(form_wiper);
    if (!body) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, std::string{operation_name(operation)} +
                                                      " request body could not be prepared"));
    }
    return std::move(*body);
}

[[nodiscard]] std::expected<ports::HttpRequest, TwitchError>
make_oauth_post(std::string_view url, std::string& form, TwitchOperation operation) {
    auto body = make_form_body(form, operation);
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }
    ports::HttpRequest request;
    request.method = ports::HttpMethod::Post;
    request.url = url;
    request.headers = {
        ports::HttpHeader{
            .name = "Accept",
            .value = "application/json",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        },
        ports::HttpHeader{
            .name = "Content-Type",
            .value = "application/x-www-form-urlencoded",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        },
    };
    request.body = std::move(*body);
    request.timeouts = ports::HttpTimeouts{
        .connect = 10s,
        .operation = 30s,
        .stalled_transfer = 30s,
    };
    request.response_limits = ports::HttpResponseLimits{
        .max_header_bytes = ports::max_http_response_header_bytes,
        .max_body_bytes = max_oauth_response_bytes,
    };
    return request;
}

[[nodiscard]] ports::HttpHeader authorization_header(std::string_view prefix,
                                                     const support::SecretValue& token) {
    ports::HttpHeader header{
        .name = "Authorization",
        .value = std::string{prefix},
        .sensitivity = ports::HttpHeaderSensitivity::Sensitive,
    };
    header.value.reserve(prefix.size() + token.size());
    std::ranges::transform(token.bytes(), std::back_inserter(header.value), [](std::byte byte) {
        return static_cast<char>(std::to_integer<unsigned char>(byte));
    });
    return header;
}

[[nodiscard]] std::string_view response_document(const ports::HttpResponse& response) noexcept {
    return {reinterpret_cast<const char*>(response.body.data()), response.body.size()};
}

[[nodiscard]] std::expected<TwitchTokenGrant, TwitchError>
decode_token_grant(ports::HttpResponse& response, TwitchOperation operation) {
    ResponseBodyWiper body_wiper{response.body};
    detail::ParsedTwitchTokenGrant parsed;
    TokenResponseWiper parsed_wiper{parsed};
    if (const auto parse_error =
            glz::read<ResponseReadOptions{}>(parsed, response_document(response));
        parse_error) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed,
                       std::string{operation_name(operation)} + " returned invalid JSON",
                       std::nullopt, std::nullopt, response.status_code));
    }
    if (!parsed.access_token || !parsed.refresh_token || !parsed.expires_in || !parsed.scope ||
        !parsed.token_type || !valid_secret(*parsed.access_token) ||
        !valid_secret(*parsed.refresh_token) || !valid_duration(*parsed.expires_in) ||
        !ascii_case_equal(*parsed.token_type, "bearer")) {
        return std::unexpected(make_error(TwitchDisposition::Failed,
                                          std::string{operation_name(operation)} +
                                              " returned an incomplete token grant",
                                          std::nullopt, std::nullopt, response.status_code));
    }
    auto scopes = take_scopes(*parsed.scope);
    if (!valid_scopes(scopes)) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed,
                       std::string{operation_name(operation)} + " returned an invalid scope grant",
                       std::nullopt, std::nullopt, response.status_code));
    }

    auto access_token = support::SecretValue::from_text(*parsed.access_token);
    auto refresh_token = support::SecretValue::from_text(*parsed.refresh_token);
    return TwitchTokenGrant{
        .access_token = std::move(access_token),
        .refresh_token = std::move(refresh_token),
        .expires_in = std::chrono::seconds{*parsed.expires_in},
        .scopes = std::move(scopes),
    };
}

[[nodiscard]] bool authorization_pending(const ports::HttpResponse& response) {
    detail::ParsedTwitchError parsed;
    if (const auto parse_error =
            glz::read<ResponseReadOptions{}>(parsed, response_document(response));
        parse_error) {
        return false;
    }
    return parsed.status == 400 && parsed.message == "authorization_pending";
}

[[nodiscard]] bool valid_chat_message(std::string_view message) noexcept {
    const auto code_points = support::utf8_code_point_count(message);
    return !message.empty() && message.size() <= max_twitch_chat_characters * 4 && code_points &&
           *code_points <= max_twitch_chat_characters &&
           std::ranges::none_of(message, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20U || byte == 0x7fU;
           });
}

} // namespace

std::expected<TwitchClient, TwitchError> TwitchClient::create(const ports::IHttpClient& http_client,
                                                              std::string client_id) {
    if (!valid_client_id(client_id)) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, "The Twitch application client ID is invalid"));
    }
    return TwitchClient{http_client, std::move(client_id)};
}

TwitchClient::TwitchClient(const ports::IHttpClient& http_client, std::string client_id) noexcept
    : http_client_{http_client}, client_id_{std::move(client_id)} {}

std::expected<TwitchDeviceAuthorization, TwitchError>
TwitchClient::start_device_authorization(const std::stop_token& stop_token) const {
    constexpr auto operation = TwitchOperation::StartDeviceAuthorization;
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(
                make_error(TwitchDisposition::Cancelled, "Twitch authorization was cancelled"));
        }
        std::string form;
        append_form_field(form, FormFieldName{"client_id"}, client_id_);
        append_form_field(form, FormFieldName{"scopes"}, twitch_chat_scope);
        auto request = make_oauth_post(device_url, form, operation);
        if (!request) {
            return std::unexpected(std::move(request.error()));
        }
        auto response = http_client_.execute(std::move(*request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), operation));
        }
        ResponseBodyWiper body_wiper{response->body};
        if (response->status_code != 200) {
            return std::unexpected(classify_status(*response, operation));
        }

        detail::ParsedTwitchDeviceAuthorization parsed;
        DeviceResponseWiper parsed_wiper{parsed};
        if (const auto parse_error =
                glz::read<ResponseReadOptions{}>(parsed, response_document(*response));
            parse_error || !parsed.device_code || !parsed.expires_in || !parsed.interval ||
            !parsed.user_code || !parsed.verification_uri || !valid_secret(*parsed.device_code) ||
            !valid_duration(*parsed.expires_in) || *parsed.interval <= 0 ||
            *parsed.interval > 300 || !safe_visible_text(*parsed.user_code, 64) ||
            !valid_verification_uri(*parsed.verification_uri)) {
            return std::unexpected(make_error(TwitchDisposition::Failed,
                                              "Twitch authorization returned an invalid response",
                                              std::nullopt, std::nullopt, response->status_code));
        }

        auto device_code = support::SecretValue::from_text(*parsed.device_code);
        return TwitchDeviceAuthorization{
            .device_code = std::move(device_code),
            .user_code = std::move(*parsed.user_code),
            .verification_uri = std::move(*parsed.verification_uri),
            .expires_in = std::chrono::seconds{*parsed.expires_in},
            .polling_interval = std::chrono::seconds{*parsed.interval},
        };
    } catch (...) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, "Twitch authorization failed unexpectedly"));
    }
}

std::expected<TwitchDevicePollResult, TwitchError>
TwitchClient::poll_device_authorization(const support::SecretValue& device_code,
                                        const std::stop_token& stop_token) const {
    constexpr auto operation = TwitchOperation::PollDeviceAuthorization;
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(TwitchDisposition::Cancelled,
                                              "Twitch authorization polling was cancelled"));
        }
        if (!valid_secret(device_code)) {
            return std::unexpected(make_error(TwitchDisposition::Failed,
                                              "The Twitch device authorization is invalid"));
        }
        std::string form;
        append_form_field(form, FormFieldName{"client_id"}, client_id_);
        append_form_field(form, FormFieldName{"scopes"}, twitch_chat_scope);
        append_form_field(form, FormFieldName{"device_code"}, device_code);
        append_form_field(form, FormFieldName{"grant_type"}, device_grant_type);
        auto request = make_oauth_post(token_url, form, operation);
        if (!request) {
            return std::unexpected(std::move(request.error()));
        }
        auto response = http_client_.execute(std::move(*request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), operation));
        }
        if (response->status_code != 200) {
            ResponseBodyWiper body_wiper{response->body};
            if (response->status_code == 400 && authorization_pending(*response)) {
                return TwitchAuthorizationPending{};
            }
            return std::unexpected(classify_status(*response, operation));
        }
        auto grant = decode_token_grant(*response, operation);
        if (!grant) {
            return std::unexpected(std::move(grant.error()));
        }
        return TwitchDevicePollResult{std::in_place_type<TwitchTokenGrant>, std::move(*grant)};
    } catch (...) {
        return std::unexpected(make_error(TwitchDisposition::Failed,
                                          "Twitch authorization polling failed unexpectedly"));
    }
}

std::expected<TwitchValidatedIdentity, TwitchError>
TwitchClient::validate_access_token(const support::SecretValue& access_token,
                                    const std::stop_token& stop_token) const {
    constexpr auto operation = TwitchOperation::ValidateAccessToken;
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(TwitchDisposition::Cancelled,
                                              "Twitch session validation was cancelled"));
        }
        if (!valid_secret(access_token)) {
            return std::unexpected(
                make_error(TwitchDisposition::Reconnect, "The Twitch session is invalid"));
        }
        ports::HttpRequest request;
        request.method = ports::HttpMethod::Get;
        request.url = validate_url;
        request.headers = {
            ports::HttpHeader{
                .name = "Accept",
                .value = "application/json",
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
            authorization_header("OAuth ", access_token),
        };
        request.timeouts =
            ports::HttpTimeouts{.connect = 10s, .operation = 30s, .stalled_transfer = 30s};
        request.response_limits = ports::HttpResponseLimits{
            .max_header_bytes = ports::max_http_response_header_bytes,
            .max_body_bytes = max_oauth_response_bytes,
        };
        auto response = http_client_.execute(std::move(request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), operation));
        }
        if (response->status_code != 200) {
            ResponseBodyWiper body_wiper{response->body};
            return std::unexpected(classify_status(*response, operation));
        }

        detail::ParsedTwitchValidation parsed;
        if (const auto parse_error =
                glz::read<ResponseReadOptions{}>(parsed, response_document(*response));
            parse_error || !parsed.client_id || !parsed.login || !parsed.scopes ||
            !parsed.user_id || !parsed.expires_in || *parsed.client_id != client_id_ ||
            !valid_login(*parsed.login) || !valid_user_id(*parsed.user_id) ||
            !valid_duration(*parsed.expires_in) || !valid_scopes(*parsed.scopes)) {
            return std::unexpected(make_error(TwitchDisposition::Reconnect,
                                              "Twitch returned an invalid session identity",
                                              std::nullopt, std::nullopt, response->status_code));
        }
        return TwitchValidatedIdentity{
            .user_id = std::move(*parsed.user_id),
            .login = std::move(*parsed.login),
            .expires_in = std::chrono::seconds{*parsed.expires_in},
            .scopes = std::move(*parsed.scopes),
        };
    } catch (...) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, "Twitch session validation failed unexpectedly"));
    }
}

std::expected<TwitchTokenGrant, TwitchError>
TwitchClient::refresh_access_token(const support::SecretValue& refresh_token,
                                   const std::stop_token& stop_token) const {
    constexpr auto operation = TwitchOperation::RefreshAccessToken;
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(
                make_error(TwitchDisposition::Cancelled, "Twitch session refresh was cancelled"));
        }
        if (!valid_secret(refresh_token)) {
            return std::unexpected(
                make_error(TwitchDisposition::Reconnect, "The Twitch refresh token is invalid"));
        }
        std::string form;
        append_form_field(form, FormFieldName{"grant_type"}, "refresh_token");
        append_form_field(form, FormFieldName{"refresh_token"}, refresh_token);
        append_form_field(form, FormFieldName{"client_id"}, client_id_);
        auto request = make_oauth_post(token_url, form, operation);
        if (!request) {
            return std::unexpected(std::move(request.error()));
        }
        auto response = http_client_.execute(std::move(*request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), operation));
        }
        if (response->status_code != 200) {
            ResponseBodyWiper body_wiper{response->body};
            return std::unexpected(classify_status(*response, operation));
        }
        return decode_token_grant(*response, operation);
    } catch (...) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, "Twitch session refresh failed unexpectedly"));
    }
}

std::expected<void, TwitchError>
TwitchClient::revoke_access_token(const support::SecretValue& access_token,
                                  const std::stop_token& stop_token) const {
    constexpr auto operation = TwitchOperation::RevokeAccessToken;
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(
                make_error(TwitchDisposition::Cancelled, "Twitch disconnect was cancelled"));
        }
        if (!valid_secret(access_token)) {
            return std::unexpected(
                make_error(TwitchDisposition::Failed, "The Twitch access token is invalid"));
        }
        std::string form;
        append_form_field(form, FormFieldName{"client_id"}, client_id_);
        append_form_field(form, FormFieldName{"token"}, access_token);
        auto request = make_oauth_post(revoke_url, form, operation);
        if (!request) {
            return std::unexpected(std::move(request.error()));
        }
        auto response = http_client_.execute(std::move(*request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), operation));
        }
        if (response->status_code != 200) {
            return std::unexpected(classify_status(*response, operation));
        }
        return {};
    } catch (...) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, "Twitch disconnect failed unexpectedly"));
    }
}

std::expected<TwitchChatResult, TwitchError>
TwitchClient::send_chat_message(std::string_view authenticated_user_id, std::string_view message,
                                const support::SecretValue& access_token,
                                const std::stop_token& stop_token) const {
    constexpr auto operation = TwitchOperation::SendChatMessage;
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(
                make_error(TwitchDisposition::Cancelled, "Twitch chat delivery was cancelled"));
        }
        if (!valid_user_id(authenticated_user_id) || !valid_chat_message(message) ||
            !valid_secret(access_token)) {
            return std::unexpected(
                make_error(TwitchDisposition::Failed, "The Twitch chat request is invalid"));
        }
        detail::TwitchChatRequestBody body{
            .broadcaster_id = std::string{authenticated_user_id},
            .sender_id = std::string{authenticated_user_id},
            .message = std::string{message},
        };
        std::string document;
        if (const auto write_error = glz::write<RequestWriteOptions{}>(body, document);
            write_error) {
            return std::unexpected(make_error(TwitchDisposition::Failed,
                                              "The Twitch chat request could not be serialized"));
        }
        const auto document_bytes = std::as_bytes(std::span{document.data(), document.size()});

        ports::HttpRequest request;
        request.method = ports::HttpMethod::Post;
        request.url = chat_url;
        request.headers = {
            ports::HttpHeader{
                .name = "Accept",
                .value = "application/json",
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
            ports::HttpHeader{
                .name = "Client-Id",
                .value = client_id_,
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
            ports::HttpHeader{
                .name = "Content-Type",
                .value = "application/json",
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
            authorization_header("Bearer ", access_token),
        };
        request.body = http::make_memory_http_body_source(
            std::vector<std::byte>{document_bytes.begin(), document_bytes.end()});
        request.timeouts =
            ports::HttpTimeouts{.connect = 10s, .operation = 30s, .stalled_transfer = 30s};
        request.response_limits = ports::HttpResponseLimits{
            .max_header_bytes = ports::max_http_response_header_bytes,
            .max_body_bytes = max_chat_response_bytes,
        };
        auto response = http_client_.execute(std::move(request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), operation));
        }
        if (response->status_code != 200) {
            return std::unexpected(classify_status(*response, operation));
        }

        detail::ParsedTwitchChatResponse parsed;
        if (const auto parse_error =
                glz::read<ResponseReadOptions{}>(parsed, response_document(*response));
            parse_error || !parsed.data || parsed.data->size() != 1 ||
            !parsed.data->front().is_sent) {
            return std::unexpected(make_error(TwitchDisposition::Failed,
                                              "Twitch returned an invalid chat response",
                                              std::nullopt, std::nullopt, response->status_code));
        }
        auto& item = parsed.data->front();
        if (item.is_sent.value_or(false)) {
            if (!item.message_id || !safe_visible_text(*item.message_id, 256) || item.drop_reason) {
                return std::unexpected(make_error(
                    TwitchDisposition::Failed, "Twitch returned an invalid chat response",
                    std::nullopt, std::nullopt, response->status_code));
            }
            return TwitchChatResult{
                .is_sent = true,
                .message_id = std::move(item.message_id),
                .drop_reason = std::nullopt,
            };
        }

        if (item.message_id || !item.drop_reason || !item.drop_reason->code ||
            !item.drop_reason->message || !safe_visible_text(*item.drop_reason->code, 128) ||
            !safe_visible_text(*item.drop_reason->message, 1024)) {
            return std::unexpected(make_error(TwitchDisposition::Failed,
                                              "Twitch returned an invalid chat response",
                                              std::nullopt, std::nullopt, response->status_code));
        }
        return TwitchChatResult{
            .is_sent = false,
            .message_id = std::nullopt,
            .drop_reason =
                TwitchDropReason{
                    .code = std::move(*item.drop_reason->code),
                    .message = std::move(*item.drop_reason->message),
                },
        };
    } catch (...) {
        return std::unexpected(
            make_error(TwitchDisposition::Failed, "Twitch chat delivery failed unexpectedly"));
    }
}

const std::string& TwitchClient::client_id() const noexcept {
    return client_id_;
}

} // namespace manny_uploader::providers
