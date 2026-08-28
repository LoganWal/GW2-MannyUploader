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
    struct ParsedDiscordDelivery {
        std::optional<bool> enabled;
        std::optional<bool> defaultsAvailable;
        std::optional<bool> channelOverrideAllowed;
        std::optional<std::vector<std::string>> enabledMessageKinds;
        struct ParsedChannel {
            std::optional<std::string> channelId;
            std::optional<std::string> channelName;
        };
        std::optional<std::vector<ParsedChannel>> channels;
    };
    std::optional<ParsedDiscordDelivery> discordDelivery;
};

struct ParsedDonBotVerification {
    std::optional<std::string> accountName;
    std::optional<std::vector<std::string>> capabilities;
    std::optional<std::vector<ParsedDonBotGuild>> guilds;
};

struct ParsedDonBotDiscordDelivery {
    std::optional<bool> requested;
    std::optional<std::string> outcome;
    std::optional<std::uint64_t> sent;
    std::optional<std::uint64_t> skipped;
    std::optional<std::uint64_t> failed;
    std::optional<std::uint64_t> ambiguous;
};

struct ParsedDonBotProgress {
    std::optional<std::string> stage;
    std::optional<std::string> message;
    std::optional<std::uint64_t> fightLogId;
    std::optional<ParsedDonBotDiscordDelivery> discordDelivery;
};

struct DonBotPermalinkDeliveryRequest {
    std::string mode;
    std::optional<std::string> channelId;
};

struct DonBotPermalinkImportRequest {
    std::string url;
    std::string guildId;
    std::optional<DonBotPermalinkDeliveryRequest> discordDelivery;
};

struct ParsedDonBotPermalinkImport {
    std::optional<std::uint64_t> uploadId;
    std::optional<std::uint64_t> fightLogId;
    std::optional<std::string> status;
    std::optional<bool> duplicate;
    std::optional<bool> discordDeliveryAccepted;
    std::optional<ParsedDonBotDiscordDelivery> discordDelivery;
};

} // namespace detail

namespace {

using namespace std::chrono_literals;

constexpr std::string_view tus_version = "1.0.0";
constexpr std::string_view guilds_path = "/api/upload/gw2/guilds";
constexpr std::string_view tus_path = "/api/upload/tus";
constexpr std::string_view permalink_import_path = "/api/upload/gw2/url";
constexpr std::string_view progress_path = "/api/upload/stream/";
constexpr std::size_t max_api_base_bytes = 2048;
constexpr std::size_t max_display_name_bytes = 256;
constexpr std::size_t max_guilds = 256;
constexpr std::size_t max_channels_per_guild = 256;
constexpr std::size_t max_total_channels = 2048;
constexpr std::size_t max_delivery_messages = 4;
constexpr std::string_view discord_delivery_capability = "discord-summary-delivery-v1";
constexpr std::array<std::string_view, 4> discord_delivery_message_kinds{
    "pve-summary",
    "wvw-summary",
    "wvw-advanced",
    "wvw-stream",
};
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

[[nodiscard]] bool trusted_dps_report_permalink(std::string_view value) noexcept {
    constexpr std::string_view prefix = "https://dps.report/";
    return value.starts_with(prefix) && value.size() > prefix.size() && value.size() <= 2048 &&
           !value.contains('@') && !value.contains('?') && !value.contains('#') &&
           !value.contains('\\') && visible_ascii(value);
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

[[nodiscard]] bool
valid_discord_delivery_request(const DonBotDiscordDeliveryRequest& request) noexcept {
    switch (request.mode) {
    case domain::DonBotDiscordDeliveryMode::None:
    case domain::DonBotDiscordDeliveryMode::GuildDefaults:
        return request.channel_id.empty();
    case domain::DonBotDiscordDeliveryMode::ChannelOverride:
        return valid_positive_long(request.channel_id);
    }
    return false;
}

[[nodiscard]] bool delivery_requested(const DonBotDiscordDeliveryRequest& request) noexcept {
    return request.mode != domain::DonBotDiscordDeliveryMode::None;
}

[[nodiscard]] std::string_view delivery_mode_name(domain::DonBotDiscordDeliveryMode mode) noexcept {
    switch (mode) {
    case domain::DonBotDiscordDeliveryMode::GuildDefaults:
        return "guild_defaults";
    case domain::DonBotDiscordDeliveryMode::ChannelOverride:
        return "channel_override";
    case domain::DonBotDiscordDeliveryMode::None:
        break;
    }
    return {};
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

    const auto capability_present =
        parsed.capabilities &&
        std::ranges::find(*parsed.capabilities, discord_delivery_capability) !=
            parsed.capabilities->end();
    DonBotVerification result{
        .account_name = std::move(*parsed.accountName),
        .discord_summary_delivery_v1 = capability_present,
        .guilds = {},
    };
    result.guilds.reserve(parsed.guilds->size());
    std::unordered_set<std::string> identifiers;
    identifiers.reserve(parsed.guilds->size());
    std::size_t total_channels{};
    for (auto& guild : *parsed.guilds) {
        auto guild_id = std::move(guild.guildId).value_or(std::string{});
        auto guild_name = std::move(guild.guildName).value_or(std::string{});
        if (!valid_positive_long(guild_id) || !valid_display_name(guild_name) ||
            !identifiers.insert(guild_id).second) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an invalid guild list", std::nullopt,
                                              std::nullopt, response.status_code));
        }
        ports::DonBotDiscordDeliveryPolicy delivery;
        if (capability_present) {
            if (!guild.discordDelivery || !guild.discordDelivery->enabled ||
                !guild.discordDelivery->defaultsAvailable ||
                !guild.discordDelivery->channelOverrideAllowed ||
                !guild.discordDelivery->enabledMessageKinds || !guild.discordDelivery->channels ||
                guild.discordDelivery->enabledMessageKinds->size() >
                    discord_delivery_message_kinds.size() ||
                (*guild.discordDelivery->enabled &&
                 guild.discordDelivery->enabledMessageKinds->empty()) ||
                guild.discordDelivery->channels->size() > max_channels_per_guild ||
                total_channels > max_total_channels - guild.discordDelivery->channels->size()) {
                return std::unexpected(make_error(
                    DonBotDisposition::Failed, "DonBot returned an invalid Discord delivery policy",
                    std::nullopt, std::nullopt, response.status_code));
            }
            delivery.enabled = *guild.discordDelivery->enabled;
            delivery.defaults_available = *guild.discordDelivery->defaultsAvailable;
            delivery.channel_override_allowed = *guild.discordDelivery->channelOverrideAllowed;
            std::unordered_set<std::string> kinds;
            for (const auto& kind : *guild.discordDelivery->enabledMessageKinds) {
                if (std::ranges::find(discord_delivery_message_kinds, kind) ==
                        discord_delivery_message_kinds.end() ||
                    !kinds.insert(kind).second) {
                    return std::unexpected(
                        make_error(DonBotDisposition::Failed,
                                   "DonBot returned an invalid Discord delivery policy",
                                   std::nullopt, std::nullopt, response.status_code));
                }
                delivery.pve_summary = delivery.pve_summary || kind == "pve-summary";
                delivery.wvw_summary = delivery.wvw_summary || kind == "wvw-summary";
                delivery.wvw_advanced = delivery.wvw_advanced || kind == "wvw-advanced";
                delivery.wvw_stream = delivery.wvw_stream || kind == "wvw-stream";
            }
            delivery.channels.reserve(guild.discordDelivery->channels->size());
            std::unordered_set<std::string> channel_ids;
            for (auto& channel : *guild.discordDelivery->channels) {
                auto channel_id = std::move(channel.channelId).value_or(std::string{});
                auto channel_name = std::move(channel.channelName).value_or(std::string{});
                if (!valid_positive_long(channel_id) || !valid_display_name(channel_name) ||
                    !channel_ids.insert(channel_id).second) {
                    return std::unexpected(
                        make_error(DonBotDisposition::Failed,
                                   "DonBot returned an invalid Discord channel list", std::nullopt,
                                   std::nullopt, response.status_code));
                }
                delivery.channels.push_back(ports::DonBotChannel{
                    .channel_id = std::move(channel_id),
                    .channel_name = std::move(channel_name),
                });
            }
            total_channels += delivery.channels.size();
        }
        result.guilds.push_back(DonBotGuild{
            .guild_id = std::move(guild_id),
            .guild_name = std::move(guild_name),
            .discord_delivery = std::move(delivery),
        });
    }
    return result;
}

struct ProcessedDonBotUpload {
    std::optional<std::uint64_t> fight_log_id;
    domain::DonBotDiscordDeliveryReceipt discord_delivery;
};

[[nodiscard]] std::optional<domain::DonBotDiscordDeliveryReceipt>
decode_delivery(const detail::ParsedDonBotDiscordDelivery& parsed) {
    if (!parsed.requested || !parsed.outcome || !parsed.sent || !parsed.skipped || !parsed.failed ||
        !parsed.ambiguous || *parsed.sent > max_delivery_messages ||
        *parsed.skipped > max_delivery_messages || *parsed.failed > max_delivery_messages ||
        *parsed.ambiguous > max_delivery_messages ||
        *parsed.sent + *parsed.skipped + *parsed.failed + *parsed.ambiguous >
            max_delivery_messages) {
        return std::nullopt;
    }
    auto outcome = domain::DonBotDiscordDeliveryOutcome::NotRequested;
    if (*parsed.outcome == "sent") {
        outcome = domain::DonBotDiscordDeliveryOutcome::Sent;
    } else if (*parsed.outcome == "partial") {
        outcome = domain::DonBotDiscordDeliveryOutcome::Partial;
    } else if (*parsed.outcome == "skipped") {
        outcome = domain::DonBotDiscordDeliveryOutcome::Skipped;
    } else if (*parsed.outcome == "failed") {
        outcome = domain::DonBotDiscordDeliveryOutcome::Failed;
    } else if (*parsed.outcome == "ambiguous") {
        outcome = domain::DonBotDiscordDeliveryOutcome::Ambiguous;
    } else if (*parsed.outcome != "not_requested") {
        return std::nullopt;
    }
    if ((*parsed.requested) == (outcome == domain::DonBotDiscordDeliveryOutcome::NotRequested)) {
        return std::nullopt;
    }
    const auto populated_categories =
        static_cast<unsigned>(*parsed.sent != 0) + static_cast<unsigned>(*parsed.skipped != 0) +
        static_cast<unsigned>(*parsed.failed != 0) + static_cast<unsigned>(*parsed.ambiguous != 0);
    const auto consistent = [&] {
        switch (outcome) {
        case domain::DonBotDiscordDeliveryOutcome::NotRequested:
            return populated_categories == 0;
        case domain::DonBotDiscordDeliveryOutcome::Sent:
            return *parsed.sent != 0 && populated_categories == 1;
        case domain::DonBotDiscordDeliveryOutcome::Partial:
            return populated_categories >= 2;
        case domain::DonBotDiscordDeliveryOutcome::Skipped:
            return *parsed.skipped != 0 && populated_categories == 1;
        case domain::DonBotDiscordDeliveryOutcome::Failed:
            return *parsed.failed != 0 && populated_categories == 1;
        case domain::DonBotDiscordDeliveryOutcome::Ambiguous:
            return *parsed.ambiguous != 0 && populated_categories == 1;
        }
        return false;
    }();
    if (!consistent) {
        return std::nullopt;
    }
    return domain::DonBotDiscordDeliveryReceipt{
        .outcome = outcome,
        .sent = static_cast<std::uint16_t>(*parsed.sent),
        .skipped = static_cast<std::uint16_t>(*parsed.skipped),
        .failed = static_cast<std::uint16_t>(*parsed.failed),
        .ambiguous = static_cast<std::uint16_t>(*parsed.ambiguous),
    };
}

[[nodiscard]] bool delivery_matches_request(const domain::DonBotDiscordDeliveryReceipt& receipt,
                                            bool requested) noexcept {
    return requested == (receipt.outcome != domain::DonBotDiscordDeliveryOutcome::NotRequested);
}

[[nodiscard]] std::expected<ProcessedDonBotUpload, DonBotError>
decode_progress(const ports::HttpResponse& response, bool require_delivery) {
    const auto document =
        std::string_view{reinterpret_cast<const char*>(response.body.data()), response.body.size()};
    std::optional<std::uint64_t> fight_log_id;
    bool last_event_complete{};
    domain::DonBotDiscordDeliveryReceipt discord_delivery;
    bool delivery_seen{};
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
        last_event_complete = *parsed.stage == "complete";
        if (last_event_complete) {
            if (require_delivery && !parsed.discordDelivery) {
                return std::unexpected(
                    make_error(DonBotDisposition::Failed, "DonBot did not confirm Discord delivery",
                               std::nullopt, std::nullopt, response.status_code));
            }
            if (parsed.discordDelivery) {
                auto decoded = decode_delivery(*parsed.discordDelivery);
                if (!decoded || !delivery_matches_request(*decoded, require_delivery)) {
                    return std::unexpected(
                        make_error(DonBotDisposition::Failed,
                                   "DonBot returned invalid Discord delivery progress",
                                   std::nullopt, std::nullopt, response.status_code));
                }
                discord_delivery = *decoded;
                delivery_seen = true;
            }
        } else if (parsed.discordDelivery) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed,
                           "DonBot returned Discord delivery before upload completion",
                           std::nullopt, std::nullopt, response.status_code));
        }
    }
    if (!last_event_complete) {
        return std::unexpected(make_error(DonBotDisposition::Failed,
                                          "DonBot upload progress ended before completion",
                                          std::nullopt, std::nullopt, response.status_code));
    }
    if (require_delivery && !delivery_seen) {
        return std::unexpected(make_error(DonBotDisposition::Failed,
                                          "DonBot did not confirm Discord delivery", std::nullopt,
                                          std::nullopt, response.status_code));
    }
    return ProcessedDonBotUpload{
        .fight_log_id = fight_log_id,
        .discord_delivery = discord_delivery,
    };
}

[[nodiscard]] ports::HttpTimeouts upload_timeouts();
[[nodiscard]] ports::HttpResponseLimits small_response_limits(std::size_t body_bytes);

[[nodiscard]] std::expected<ProcessedDonBotUpload, DonBotError>
wait_for_processing(const ports::IHttpClient& http_client, const ParsedBaseUrl& base,
                    std::uint64_t upload_id, const support::SecretValue& gw2_api_key,
                    bool require_delivery, const std::stop_token& stop_token) {
    auto headers = common_headers(gw2_api_key);
    headers.push_back(ports::HttpHeader{
        .name = "Accept",
        .value = "text/event-stream",
        .sensitivity = ports::HttpHeaderSensitivity::Public,
    });
    ports::HttpRequest request{
        .method = ports::HttpMethod::Get,
        .url = base.normalized + std::string{progress_path} + std::to_string(upload_id),
        .headers = std::move(headers),
        .body = nullptr,
        .timeouts = upload_timeouts(),
        .response_limits = small_response_limits(std::size_t{256} * 1024U),
    };
    auto response = http_client.execute(std::move(request), stop_token);
    if (!response) {
        if (response.error().code == ports::HttpErrorCode::Cancelled) {
            return std::unexpected(classify_transport_error(response.error(), true));
        }
        if (require_delivery) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot Discord delivery status is unavailable"));
        }
        return ProcessedDonBotUpload{};
    }
    if (response->status_code != 200) {
        if (require_delivery) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot Discord delivery status is unavailable",
                                              std::nullopt, std::nullopt, response->status_code));
        }
        return ProcessedDonBotUpload{};
    }
    return decode_progress(*response, require_delivery);
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
                     const DonBotDiscordDeliveryRequest& discord_delivery,
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
        if (!valid_discord_delivery_request(discord_delivery)) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "The DonBot Discord delivery selection is invalid"));
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
        auto metadata =
            "filename dXBsb2FkLnpldnRj,guildid " + base64(guild_id) + ",wingman ZmFsc2U=";
        if (delivery_requested(discord_delivery)) {
            metadata += ",discorddelivery " + base64(delivery_mode_name(discord_delivery.mode));
            if (discord_delivery.mode == domain::DonBotDiscordDeliveryMode::ChannelOverride) {
                metadata += ",discordchannelid " + base64(discord_delivery.channel_id);
            }
        }
        create_headers.push_back(ports::HttpHeader{
            .name = "Upload-Metadata",
            .value = std::move(metadata),
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
        if (delivery_requested(discord_delivery)) {
            const auto* delivery_ack = unique_header(*created, "X-DonBot-Discord-Delivery");
            if (delivery_ack == nullptr || delivery_ack->value != "accepted" ||
                header_missing_or_duplicated(*created, "X-DonBot-Discord-Delivery")) {
                return std::unexpected(make_error(
                    DonBotDisposition::Failed, "DonBot did not accept the Discord delivery request",
                    std::nullopt, std::nullopt, created->status_code));
            }
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
        if (delivery_requested(discord_delivery) && !upload_id) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed,
                           "DonBot did not provide a Discord delivery tracking identifier",
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
        ProcessedDonBotUpload processed;
        if (upload_id) {
            auto completed = wait_for_processing(http_client_, *base, *upload_id, gw2_api_key,
                                                 delivery_requested(discord_delivery), stop_token);
            if (!completed) {
                return std::unexpected(std::move(completed.error()));
            }
            processed = std::move(*completed);
        }
        return DonBotUploadSuccess{
            .upload_id = upload_id,
            .fight_log_id = processed.fight_log_id,
            .discord_delivery = processed.discord_delivery,
        };
    } catch (...) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot upload failed unexpectedly"));
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// Ordered protocol arguments implement the existing client port.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::expected<DonBotUploadSuccess, DonBotError>
DonBotClient::import_permalink(std::string_view dps_report_permalink, std::string_view api_base_url,
                               std::string_view guild_id, const support::SecretValue& gw2_api_key,
                               const DonBotDiscordDeliveryRequest& discord_delivery,
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
        if (!trusted_dps_report_permalink(dps_report_permalink)) {
            return std::unexpected(
                make_error(DonBotDisposition::Failed, "The dps.report permalink is invalid"));
        }
        if (!valid_discord_delivery_request(discord_delivery)) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "The DonBot Discord delivery selection is invalid"));
        }

        detail::DonBotPermalinkImportRequest request_body{
            .url = std::string{dps_report_permalink},
            .guildId = std::string{guild_id},
            .discordDelivery = std::nullopt,
        };
        if (delivery_requested(discord_delivery)) {
            request_body.discordDelivery = detail::DonBotPermalinkDeliveryRequest{
                .mode = std::string{delivery_mode_name(discord_delivery.mode)},
                .channelId = discord_delivery.channel_id.empty()
                                 ? std::nullopt
                                 : std::optional<std::string>{discord_delivery.channel_id},
            };
        }
        std::string document;
        if (const auto error = glz::write_json(request_body, document); error) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "The DonBot import request could not be prepared"));
        }
        const auto body_bytes = std::as_bytes(std::span{document.data(), document.size()});
        auto headers = common_headers(gw2_api_key);
        headers.push_back(ports::HttpHeader{
            .name = "Content-Type",
            .value = "application/json",
            .sensitivity = ports::HttpHeaderSensitivity::Public,
        });
        ports::HttpRequest request{
            .method = ports::HttpMethod::Post,
            .url = base->normalized + std::string{permalink_import_path},
            .headers = std::move(headers),
            .body = http::make_memory_http_body_source(
                std::vector<std::byte>{body_bytes.begin(), body_bytes.end()}),
            .timeouts = upload_timeouts(),
            .response_limits = small_response_limits(std::size_t{64} * 1024U),
        };
        auto response = http_client_.execute(std::move(request), stop_token);
        if (!response) {
            return std::unexpected(classify_transport_error(response.error(), false));
        }
        if (response->status_code != 200 && response->status_code != 202) {
            return std::unexpected(classify_status(*response, false));
        }

        const auto response_document = std::string_view{
            reinterpret_cast<const char*>(response->body.data()), response->body.size()};
        detail::ParsedDonBotPermalinkImport parsed;
        if (const auto error = glz::read<ResponseReadOptions{}>(parsed, response_document);
            error || !parsed.uploadId || !parsed.status || !parsed.duplicate ||
            *parsed.uploadId == 0 ||
            *parsed.uploadId >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            (*parsed.status != "pending" && *parsed.status != "complete") ||
            (parsed.fightLogId &&
             (*parsed.fightLogId == 0 ||
              *parsed.fightLogId >
                  static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) ||
            (*parsed.status == "complete") != parsed.fightLogId.has_value()) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot returned an invalid import response",
                                              std::nullopt, std::nullopt, response->status_code));
        }
        if (delivery_requested(discord_delivery) &&
            (!parsed.discordDeliveryAccepted || !*parsed.discordDeliveryAccepted)) {
            return std::unexpected(make_error(DonBotDisposition::Failed,
                                              "DonBot did not accept the Discord delivery request",
                                              std::nullopt, std::nullopt, response->status_code));
        }

        auto fight_log_id = parsed.fightLogId;
        domain::DonBotDiscordDeliveryReceipt delivery_receipt;
        if (!fight_log_id) {
            auto processed = wait_for_processing(http_client_, *base, *parsed.uploadId, gw2_api_key,
                                                 delivery_requested(discord_delivery), stop_token);
            if (!processed) {
                return std::unexpected(std::move(processed.error()));
            }
            fight_log_id = processed->fight_log_id;
            delivery_receipt = processed->discord_delivery;
        } else {
            if (delivery_requested(discord_delivery) && !parsed.discordDelivery) {
                return std::unexpected(
                    make_error(DonBotDisposition::Failed, "DonBot did not confirm Discord delivery",
                               std::nullopt, std::nullopt, response->status_code));
            }
            if (parsed.discordDelivery) {
                auto decoded = decode_delivery(*parsed.discordDelivery);
                if (!decoded ||
                    !delivery_matches_request(*decoded, delivery_requested(discord_delivery))) {
                    return std::unexpected(
                        make_error(DonBotDisposition::Failed,
                                   "DonBot returned invalid Discord delivery status", std::nullopt,
                                   std::nullopt, response->status_code));
                }
                delivery_receipt = *decoded;
            }
        }
        return DonBotUploadSuccess{
            .upload_id = parsed.uploadId,
            .fight_log_id = fight_log_id,
            .discord_delivery = delivery_receipt,
        };
    } catch (...) {
        return std::unexpected(
            make_error(DonBotDisposition::Failed, "The DonBot import failed unexpectedly"));
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace manny_uploader::providers
