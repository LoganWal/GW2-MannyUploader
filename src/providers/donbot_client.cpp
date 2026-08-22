#include "manny_uploader/providers/donbot_client.hpp"

#include "manny_uploader/http/body_sources.hpp"
#include "manny_uploader/support/utf8.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace manny_uploader::providers {
namespace detail {

struct ParsedDonBotGuild {
    std::optional<std::string> guildId;
    std::optional<std::string> guildName;
};

struct ParsedDonBotVerification {
    std::optional<std::string> accountName;
    std::optional<std::vector<ParsedDonBotGuild>> guilds;
};

struct ParsedDonBotProgress {
    std::optional<std::string> stage;
    std::optional<std::string> message;
    std::optional<std::uint64_t> fightLogId;
};

} // namespace detail

namespace {

using namespace std::chrono_literals;

constexpr std::string_view tus_version = "1.0.0";
constexpr std::string_view guilds_path = "/api/upload/gw2/guilds";
constexpr std::string_view tus_path = "/api/upload/tus";
constexpr std::string_view progress_path = "/api/upload/stream/";
constexpr std::size_t max_api_base_bytes = 2048;
constexpr std::size_t max_display_name_bytes = 256;
constexpr std::size_t max_guilds = 256;
constexpr auto default_retry_delay = 30s;
constexpr auto rate_limit_retry_delay = 60s;
constexpr auto maximum_retry_after = 900s;

struct ResponseReadOptions : glz::opts {
    bool error_on_unknown_keys{false};
    bool validate_trailing_whitespace{true};
    bool validate_skipped{true};
};

struct ParsedBaseUrl {
    std::string normalized;
    std::string origin;
};

[[nodiscard]] DonBotError make_error(DonBotDisposition disposition, std::string detail,
                                     std::optional<std::chrono::seconds> retry_after = std::nullopt,
                                     std::optional<ports::HttpErrorCode> http_error = std::nullopt,
                                     std::optional<std::uint16_t> http_status = std::nullopt) {
    return DonBotError{
        .disposition = disposition,
        .detail = std::move(detail),
        .retry_after = retry_after,
        .http_error = http_error,
        .http_status = http_status,
    };
}

[[nodiscard]] char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right, [](char lhs, char rhs) {
               return ascii_lower(lhs) == ascii_lower(rhs);
           });
}

[[nodiscard]] bool visible_ascii(std::string_view value) noexcept {
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21U && byte <= 0x7eU;
    });
}

[[nodiscard]] bool valid_api_key(const support::SecretValue& value) noexcept {
    if (value.empty() || value.size() > max_donbot_api_key_bytes) {
        return false;
    }
    return std::ranges::all_of(value.bytes(), [](std::byte byte) {
        const auto character = std::to_integer<unsigned char>(byte);
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}

[[nodiscard]] bool valid_positive_long(std::string_view value,
                                       std::uint64_t* parsed_value = nullptr) noexcept {
    if (value.empty() || value.size() > 19 || value.front() == '0') {
        return false;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0 ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    if (parsed_value != nullptr) {
        *parsed_value = parsed;
    }
    return true;
}

[[nodiscard]] bool valid_display_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= max_display_name_bytes &&
           support::is_valid_utf8(value) && std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20U || byte == 0x7fU;
           });
}

[[nodiscard]] std::expected<ParsedBaseUrl, DonBotError> parse_base_url(std::string_view value) {
    if (value.empty() || value.size() > max_api_base_bytes || !value.starts_with("https://") ||
        !visible_ascii(value) || value.contains('@') || value.contains('?') ||
        value.contains('#')) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot API endpoint is invalid"));
    }

    constexpr std::size_t scheme_size = 8;
    const auto path_start = value.find('/', scheme_size);
    const auto authority = path_start == std::string_view::npos
                               ? value.substr(scheme_size)
                               : value.substr(scheme_size, path_start - scheme_size);
    if (authority.empty() || authority.front() == '.' || authority.back() == '.' ||
        authority.contains('\\')) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot API endpoint is invalid"));
    }

    auto normalized = std::string{value};
    while (normalized.size() > scheme_size && normalized.ends_with('/')) {
        normalized.pop_back();
    }
    if (normalized.find("/../", scheme_size) != std::string::npos || normalized.ends_with("/..") ||
        normalized.find("/./", scheme_size) != std::string::npos || normalized.ends_with("/.")) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot API endpoint is invalid"));
    }
    return ParsedBaseUrl{
        .normalized = std::move(normalized),
        .origin = "https://" + std::string{authority},
    };
}

void append_secret_header(std::vector<ports::HttpHeader>& headers,
                          const support::SecretValue& api_key) {
    headers.push_back(ports::HttpHeader{
        .name = "X-GW2-API-Key",
        .value = {},
        .sensitivity = ports::HttpHeaderSensitivity::Sensitive,
    });
    auto& value = headers.back().value;
    value.resize(api_key.size());
    std::ranges::transform(api_key.bytes(), value.begin(), [](std::byte byte) {
        return static_cast<char>(std::to_integer<unsigned char>(byte));
    });
}

[[nodiscard]] std::vector<ports::HttpHeader> common_headers(const support::SecretValue& api_key) {
    std::vector<ports::HttpHeader> headers;
    headers.reserve(8);
    headers.push_back(ports::HttpHeader{
        .name = "Accept",
        .value = "application/json",
        .sensitivity = ports::HttpHeaderSensitivity::Public,
    });
    append_secret_header(headers, api_key);
    return headers;
}

[[nodiscard]] support::SecretValue verification_body(const support::SecretValue& api_key) {
    constexpr std::string_view prefix = R"({"apiKey":")";
    constexpr std::string_view suffix = R"("})";
    std::vector<std::byte> body;
    body.reserve(prefix.size() + api_key.size() + suffix.size());
    const auto append_text = [&body](std::string_view text) {
        const auto characters = std::span{text.data(), text.size()};
        const auto bytes = std::as_bytes(characters);
        body.insert(body.end(), bytes.begin(), bytes.end());
    };
    append_text(prefix);
    body.insert(body.end(), api_key.bytes().begin(), api_key.bytes().end());
    append_text(suffix);
    return support::SecretValue{std::move(body)};
}

[[nodiscard]] std::string base64(std::string_view value) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((value.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < value.size(); index += 3) {
        const auto first = static_cast<unsigned char>(value[index]);
        const auto second =
            index + 1 < value.size() ? static_cast<unsigned char>(value[index + 1]) : 0U;
        const auto third =
            index + 2 < value.size() ? static_cast<unsigned char>(value[index + 2]) : 0U;
        const auto packed = (static_cast<std::uint32_t>(first) << 16U) |
                            (static_cast<std::uint32_t>(second) << 8U) |
                            static_cast<std::uint32_t>(third);
        result.push_back(alphabet[(packed >> 18U) & 0x3fU]);
        result.push_back(alphabet[(packed >> 12U) & 0x3fU]);
        result.push_back(index + 1 < value.size() ? alphabet[(packed >> 6U) & 0x3fU] : '=');
        result.push_back(index + 2 < value.size() ? alphabet[packed & 0x3fU] : '=');
    }
    return result;
}

[[nodiscard]] const ports::HttpHeader* unique_header(const ports::HttpResponse& response,
                                                     std::string_view name) noexcept {
    const ports::HttpHeader* found{};
    for (const auto& header : response.headers) {
        if (!ascii_case_equal(header.name, name)) {
            continue;
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = &header;
    }
    return found;
}

[[nodiscard]] bool header_missing_or_duplicated(const ports::HttpResponse& response,
                                                std::string_view name) noexcept {
    std::size_t count{};
    for (const auto& header : response.headers) {
        count += ascii_case_equal(header.name, name) ? 1U : 0U;
    }
    return count != 1;
}

[[nodiscard]] std::optional<std::chrono::seconds>
numeric_retry_after(const ports::HttpResponse& response) noexcept {
    const auto* header = unique_header(response, "Retry-After");
    if (header == nullptr || header->value.empty()) {
        return std::nullopt;
    }
    std::uint32_t seconds{};
    const auto parsed =
        std::from_chars(header->value.data(), header->value.data() + header->value.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != header->value.data() + header->value.size() ||
        seconds == 0 || seconds > static_cast<std::uint32_t>(maximum_retry_after.count())) {
        return std::nullopt;
    }
    return std::chrono::seconds{seconds};
}

[[nodiscard]] DonBotError classify_transport_error(const ports::HttpError& error,
                                                   bool upload_created) {
    if (error.code == ports::HttpErrorCode::Cancelled) {
        return make_error(DonBotDisposition::Cancelled, "The DonBot request was cancelled",
                          std::nullopt, error.code);
    }
    if (upload_created) {
        return make_error(DonBotDisposition::Failed,
                          "The DonBot upload completed with an unconfirmed result", std::nullopt,
                          error.code);
    }
    switch (error.code) {
    case ports::HttpErrorCode::Timeout:
    case ports::HttpErrorCode::NameResolutionFailed:
    case ports::HttpErrorCode::ConnectionFailed:
    case ports::HttpErrorCode::TlsFailed:
    case ports::HttpErrorCode::SendFailed:
    case ports::HttpErrorCode::ReceiveFailed:
        return make_error(DonBotDisposition::Retry, "The DonBot service could not be reached",
                          default_retry_delay, error.code);
    case ports::HttpErrorCode::InvalidRequest:
    case ports::HttpErrorCode::BodyReadFailed:
    case ports::HttpErrorCode::ResponseTooLarge:
    case ports::HttpErrorCode::ProtocolError:
    case ports::HttpErrorCode::UnsupportedEnvironment:
    case ports::HttpErrorCode::InitializationFailed:
    case ports::HttpErrorCode::Internal:
        return make_error(DonBotDisposition::Failed, "The DonBot request could not be completed",
                          std::nullopt, error.code);
    case ports::HttpErrorCode::Cancelled:
        break;
    }
    return make_error(DonBotDisposition::Failed, "The DonBot request could not be completed",
                      std::nullopt, error.code);
}

[[nodiscard]] DonBotError classify_status(const ports::HttpResponse& response,
                                          bool upload_created) {
    if (upload_created) {
        return make_error(DonBotDisposition::Failed,
                          "The DonBot upload completed with an unconfirmed result", std::nullopt,
                          std::nullopt, response.status_code);
    }
    if (response.status_code == 408) {
        return make_error(DonBotDisposition::Retry, "DonBot timed out while processing the request",
                          default_retry_delay, std::nullopt, response.status_code);
    }
    if (response.status_code == 429) {
        return make_error(DonBotDisposition::Retry, "DonBot rate limited the request",
                          numeric_retry_after(response).value_or(rate_limit_retry_delay),
                          std::nullopt, response.status_code);
    }
    if (response.status_code >= 500) {
        return make_error(DonBotDisposition::Retry, "The DonBot service is temporarily unavailable",
                          default_retry_delay, std::nullopt, response.status_code);
    }
    return make_error(DonBotDisposition::Failed, "DonBot rejected the request", std::nullopt,
                      std::nullopt, response.status_code);
}

[[nodiscard]] bool valid_tus_file_id(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 256 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

[[nodiscard]] std::optional<std::string> resolve_upload_url(const ParsedBaseUrl& base,
                                                            std::string_view create_url,
                                                            std::string_view location) {
    if (location.empty() || location.size() > ports::max_http_url_bytes ||
        !visible_ascii(location) || location.contains('?') || location.contains('#') ||
        location.contains('\\')) {
        return std::nullopt;
    }

    std::string_view file_id;
    if (location.starts_with("https://")) {
        const auto path_start = location.find('/', 8);
        const auto location_origin =
            path_start == std::string_view::npos ? location : location.substr(0, path_start);
        if (!ascii_case_equal(location_origin, base.origin) ||
            path_start == std::string_view::npos) {
            return std::nullopt;
        }
        const auto expected_prefix = std::string{create_url.substr(base.origin.size())} + "/";
        const auto path = location.substr(path_start);
        if (!path.starts_with(expected_prefix)) {
            return std::nullopt;
        }
        file_id = path.substr(expected_prefix.size());
    } else if (location.starts_with('/')) {
        const auto expected_prefix = std::string{create_url.substr(base.origin.size())} + "/";
        if (!location.starts_with(expected_prefix)) {
            return std::nullopt;
        }
        file_id = location.substr(expected_prefix.size());
    } else {
        file_id = location;
    }
    if (!valid_tus_file_id(file_id)) {
        return std::nullopt;
    }
    return std::string{create_url} + "/" + std::string{file_id};
}

[[nodiscard]] std::expected<DonBotVerification, DonBotError>
decode_verification(const ports::HttpResponse& response) {
    const auto document =
        std::string_view{reinterpret_cast<const char*>(response.body.data()), response.body.size()};
    detail::ParsedDonBotVerification parsed;
    if (const auto error = glz::read<ResponseReadOptions{}>(parsed, document); error) {
        return std::unexpected(make_error(DonBotDisposition::Failed,
                                          "DonBot returned invalid verification JSON", std::nullopt,
                                          std::nullopt, response.status_code));
    }
    if (!parsed.accountName || !parsed.guilds || !valid_display_name(*parsed.accountName) ||
        parsed.guilds->size() > max_guilds) {
        return std::unexpected(make_error(DonBotDisposition::Failed,
                                          "DonBot returned an incomplete verification response",
                                          std::nullopt, std::nullopt, response.status_code));
    }

    DonBotVerification result{.account_name = std::move(*parsed.accountName), .guilds = {}};
    result.guilds.reserve(parsed.guilds->size());
    std::unordered_set<std::string> identifiers;
    identifiers.reserve(parsed.guilds->size());
    for (auto& guild : *parsed.guilds) {
        auto guild_id = std::move(guild.guildId).value_or(std::string{});
        auto guild_name = std::move(guild.guildName).value_or(std::string{});
        if (!valid_positive_long(guild_id) || !valid_display_name(guild_name) ||
            !identifiers.insert(guild_id).second) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an invalid guild list", std::nullopt,
                                              std::nullopt, response.status_code));
        }
        result.guilds.push_back(DonBotGuild{
            .guild_id = std::move(guild_id),
            .guild_name = std::move(guild_name),
        });
    }
    return result;
}

[[nodiscard]] std::expected<std::optional<std::uint64_t>, DonBotError>
decode_progress(const ports::HttpResponse& response) {
    const auto document =
        std::string_view{reinterpret_cast<const char*>(response.body.data()), response.body.size()};
    std::optional<std::uint64_t> fight_log_id;
    bool completed{};
    std::size_t offset{};
    while (offset < document.size()) {
        const auto line_end = document.find('\n', offset);
        auto line =
            document.substr(offset, line_end == std::string_view::npos ? document.size() - offset
                                                                       : line_end - offset);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        offset = line_end == std::string_view::npos ? document.size() : line_end + 1;
        if (!line.starts_with("data:")) {
            continue;
        }
        line.remove_prefix(5);
        if (line.starts_with(' ')) {
            line.remove_prefix(1);
        }
        detail::ParsedDonBotProgress parsed;
        if (const auto error = glz::read<ResponseReadOptions{}>(parsed, line);
            error || !parsed.stage) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned invalid upload progress",
                                              std::nullopt, std::nullopt, response.status_code));
        }
        if (parsed.fightLogId) {
            if (*parsed.fightLogId == 0 ||
                *parsed.fightLogId >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::unexpected(make_error(
                    DonBotDisposition::Failed, "DonBot returned an invalid fight identifier",
                    std::nullopt, std::nullopt, response.status_code));
            }
            fight_log_id = parsed.fightLogId;
        }
        if (*parsed.stage == "failed") {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot could not process the uploaded log",
                                              std::nullopt, std::nullopt, response.status_code));
        }
        completed = completed || *parsed.stage == "complete";
    }
    if (!completed) {
        return std::unexpected(make_error(DonBotDisposition::Failed,
                                          "DonBot upload progress ended before completion",
                                          std::nullopt, std::nullopt, response.status_code));
    }
    return fight_log_id;
}

[[nodiscard]] ports::HttpTimeouts upload_timeouts();
[[nodiscard]] ports::HttpResponseLimits small_response_limits(std::size_t body_bytes);

[[nodiscard]] std::expected<std::optional<std::uint64_t>, DonBotError>
wait_for_processing(const ports::IHttpClient& http_client, const ParsedBaseUrl& base,
                    std::uint64_t upload_id, const std::stop_token& stop_token) {
    ports::HttpRequest request{
        .method = ports::HttpMethod::Get,
        .url = base.normalized + std::string{progress_path} + std::to_string(upload_id),
        .headers = {ports::HttpHeader{
            .name = "Accept",
            .value = "text/event-stream",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        }},
        .body = nullptr,
        .timeouts = upload_timeouts(),
        .response_limits = small_response_limits(std::size_t{256} * 1024U),
    };
    auto response = http_client.execute(std::move(request), stop_token);
    if (!response) {
        if (response.error().code == ports::HttpErrorCode::Cancelled) {
            return std::unexpected(classify_transport_error(response.error(), true));
        }
        return std::optional<std::uint64_t>{};
    }
    if (response->status_code != 200) {
        return std::optional<std::uint64_t>{};
    }
    return decode_progress(*response);
}

[[nodiscard]] ports::HttpTimeouts upload_timeouts() {
    return ports::HttpTimeouts{
        .connect = 10s,
        .operation = 15min,
        .stalled_transfer = 15min,
    };
}

[[nodiscard]] ports::HttpResponseLimits small_response_limits(std::size_t body_bytes) {
    return ports::HttpResponseLimits{
        .max_header_bytes = ports::max_http_response_header_bytes,
        .max_body_bytes = body_bytes,
    };
}

} // namespace

DonBotClient::DonBotClient(const ports::IHttpClient& http_client) noexcept
    : http_client_{http_client} {}

std::expected<DonBotVerification, DonBotError>
DonBotClient::verify(std::string_view api_base_url, const support::SecretValue& gw2_api_key,
                     const std::stop_token& stop_token) const {
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(
                make_error(DonBotDisposition::Cancelled, "The DonBot request was cancelled"));
        }
        auto base = parse_base_url(api_base_url);
        if (!base) {
            return std::unexpected(std::move(base.error()));
        }
        if (!valid_api_key(gw2_api_key)) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed, "The DonBot GW2 API key is invalid"));
        }

        auto body_value = verification_body(gw2_api_key);
        auto body = http::make_secret_http_body_source(body_value);
        if (!body) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed,
                           "The DonBot verification request could not be prepared"));
        }

        ports::HttpRequest request;
        request.method = ports::HttpMethod::Post;
        request.url = base->normalized + std::string{guilds_path};
        request.headers = {
            ports::HttpHeader{
                .name = "Accept",
                .value = "application/json",
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
            ports::HttpHeader{
                .name = "Content-Type",
                .value = "application/json",
                .sensitivity = ports::HttpHeaderSensitivity::Public,
            },
        };
        request.body = std::move(*body);
        request.timeouts = upload_timeouts();
        request.response_limits = small_response_limits(std::size_t{256} * 1024U);

        auto response = http_client_.execute(std::move(request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), false));
        }
        if (response->status_code != 200) {
            return std::unexpected(classify_status(*response, false));
        }
        return decode_verification(*response);
    } catch (...) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot verification failed unexpectedly"));
    }
}

// Ordered protocol arguments implement the existing client port.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::expected<DonBotUploadSuccess, DonBotError>
DonBotClient::upload(const domain::LogFileIdentity& file, std::string_view api_base_url,
                     std::string_view guild_id, const support::SecretValue& gw2_api_key,
                     const std::stop_token& stop_token) const {
    try {
        if (stop_token.stop_requested()) {
            return std::unexpected(
                make_error(DonBotDisposition::Cancelled, "The DonBot request was cancelled"));
        }
        auto base = parse_base_url(api_base_url);
        if (!base) {
            return std::unexpected(std::move(base.error()));
        }
        if (!valid_positive_long(guild_id)) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed, "The DonBot guild selection is invalid"));
        }
        if (!valid_api_key(gw2_api_key)) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed, "The DonBot GW2 API key is invalid"));
        }
        if (file.size == 0 || file.size > ports::max_http_request_body_bytes) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed, "The DonBot log file size is invalid"));
        }

        const auto create_url = base->normalized + std::string{tus_path};
        auto create_headers = common_headers(gw2_api_key);
        create_headers.push_back(ports::HttpHeader{
            .name = "Tus-Resumable",
            .value = std::string{tus_version},
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        create_headers.push_back(ports::HttpHeader{
            .name = "Upload-Length",
            .value = std::to_string(file.size),
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        create_headers.push_back(ports::HttpHeader{
            .name = "Upload-Metadata",
            .value = "filename dXBsb2FkLnpldnRj,guildid " + base64(guild_id) + ",wingman ZmFsc2U=",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        ports::HttpRequest create_request{
            .method = ports::HttpMethod::Post,
            .url = create_url,
            .headers = std::move(create_headers),
            .body = nullptr,
            .timeouts = upload_timeouts(),
            .response_limits = small_response_limits(std::size_t{64} * 1024U),
        };
        auto created = http_client_.execute(std::move(create_request), stop_token);
        if (!created) {
            return std::unexpected(classify_transport_error(created.error(), false));
        }
        if (created->status_code != 201) {
            return std::unexpected(classify_status(*created, false));
        }
        const auto* location = unique_header(*created, "Location");
        const auto* created_tus = unique_header(*created, "Tus-Resumable");
        if (location == nullptr || created_tus == nullptr || created_tus->value != tus_version ||
            header_missing_or_duplicated(*created, "Location") ||
            header_missing_or_duplicated(*created, "Tus-Resumable")) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an invalid TUS creation response",
                                              std::nullopt, std::nullopt, created->status_code));
        }
        auto upload_url = resolve_upload_url(*base, create_url, location->value);
        if (!upload_url) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an unsafe TUS upload location",
                                              std::nullopt, std::nullopt, created->status_code));
        }

        std::optional<std::uint64_t> upload_id;
        std::size_t upload_id_headers{};
        for (const auto& header : created->headers) {
            if (!ascii_case_equal(header.name, "X-Log-Upload-Id")) {
                continue;
            }
            ++upload_id_headers;
            std::uint64_t parsed{};
            if (!valid_positive_long(header.value, &parsed)) {
                return std::unexpected(make_error(
                    DonBotDisposition::Failed, "DonBot returned an invalid upload identifier",
                    std::nullopt, std::nullopt, created->status_code));
            }
            upload_id = parsed;
        }
        if (upload_id_headers > 1) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an invalid upload identifier",
                                              std::nullopt, std::nullopt, created->status_code));
        }

        auto file_body =
            http::make_file_http_body_source(file.canonical_path, file.size, file.last_write_time);
        if (!file_body) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "The DonBot log file is unavailable or changed"));
        }
        auto patch_headers = common_headers(gw2_api_key);
        patch_headers.push_back(ports::HttpHeader{
            .name = "Tus-Resumable",
            .value = std::string{tus_version},
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        patch_headers.push_back(ports::HttpHeader{
            .name = "Upload-Offset",
            .value = "0",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        patch_headers.push_back(ports::HttpHeader{
            .name = "Content-Type",
            .value = "application/offset+octet-stream",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        ports::HttpRequest patch_request{
            .method = ports::HttpMethod::Patch,
            .url = std::move(*upload_url),
            .headers = std::move(patch_headers),
            .body = std::move(*file_body),
            .timeouts = upload_timeouts(),
            .response_limits = small_response_limits(std::size_t{64} * 1024U),
        };
        auto patched = http_client_.execute(std::move(patch_request), stop_token);
        if (!patched) {
            return std::unexpected(classify_transport_error(patched.error(), true));
        }
        if (patched->status_code != 204) {
            return std::unexpected(classify_status(*patched, true));
        }
        const auto* patched_tus = unique_header(*patched, "Tus-Resumable");
        const auto* final_offset = unique_header(*patched, "Upload-Offset");
        std::uint64_t parsed_offset{};
        if (patched_tus == nullptr || final_offset == nullptr ||
            patched_tus->value != tus_version ||
            header_missing_or_duplicated(*patched, "Tus-Resumable") ||
            header_missing_or_duplicated(*patched, "Upload-Offset") ||
            !valid_positive_long(final_offset->value, &parsed_offset) ||
            parsed_offset != file.size) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an invalid TUS completion response",
                                              std::nullopt, std::nullopt, patched->status_code));
        }
        std::optional<std::uint64_t> fight_log_id;
        if (upload_id) {
            auto processed = wait_for_processing(http_client_, *base, *upload_id, stop_token);
            if (!processed) {
                return std::unexpected(std::move(processed.error()));
            }
            fight_log_id = *processed;
        }
        return DonBotUploadSuccess{.upload_id = upload_id, .fight_log_id = fight_log_id};
    } catch (...) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot upload failed unexpectedly"));
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace manny_uploader::providers
