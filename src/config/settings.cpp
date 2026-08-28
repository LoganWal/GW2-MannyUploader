#include "manny_uploader/config/settings.hpp"

#include "manny_uploader/application/twitch_message_template.hpp"
#include "manny_uploader/support/utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace manny_uploader::config {
namespace {

constexpr std::size_t max_log_directory_bytes = 4096;
constexpr std::size_t max_api_url_bytes = 2048;
constexpr std::size_t max_guild_id_bytes = 19;
constexpr std::size_t max_twitch_client_id_bytes = 128;

void add_error(std::vector<SettingsValidationError>& errors, SettingsValidationErrorCode code,
               std::string field, std::string message) {
    errors.push_back(SettingsValidationError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
    });
}

[[nodiscard]] bool ascii_space_or_control(unsigned char value) noexcept {
    return value <= 0x20U || value == 0x7fU;
}

[[nodiscard]] bool has_ascii_space_or_control(std::string_view value) noexcept {
    return std::ranges::any_of(value, [](char character) {
        return ascii_space_or_control(static_cast<unsigned char>(character));
    });
}

void validate_log_directory(std::string_view value, std::vector<SettingsValidationError>& errors) {
    constexpr std::string_view field = "general.log_directory";
    if (value.empty()) {
        add_error(errors, SettingsValidationErrorCode::EmptyValue, std::string{field},
                  "Log directory must not be empty");
        return;
    }
    if (!support::is_valid_utf8(value)) {
        add_error(errors, SettingsValidationErrorCode::InvalidUtf8, std::string{field},
                  "Log directory must be valid UTF-8");
    }
    if (value.size() > max_log_directory_bytes) {
        add_error(errors, SettingsValidationErrorCode::ValueTooLong, std::string{field},
                  "Log directory exceeds 4096 bytes");
    }
    if (std::ranges::any_of(
            value, [](char character) { return static_cast<unsigned char>(character) < 0x20U; })) {
        add_error(errors, SettingsValidationErrorCode::InvalidPath, std::string{field},
                  "Log directory contains a control character");
    }
}

void validate_range(std::uint32_t value, std::uint32_t minimum, std::uint32_t maximum,
                    std::string_view field, std::vector<SettingsValidationError>& errors) {
    if (value < minimum || value > maximum) {
        add_error(errors, SettingsValidationErrorCode::OutOfRange, std::string{field},
                  std::string{field} + " is outside the supported range");
    }
}

void validate_donbot_url(std::string_view value, std::vector<SettingsValidationError>& errors) {
    constexpr std::string_view field = "donbot.api_base_url";
    const auto invalid = [&errors, field](std::string message) {
        add_error(errors, SettingsValidationErrorCode::InvalidUrl, std::string{field},
                  std::move(message));
    };

    if (value.empty() || value.size() > max_api_url_bytes || !support::is_valid_utf8(value)) {
        invalid("DonBot API base URL must be non-empty UTF-8 of at most 2048 bytes");
        return;
    }
    if (!value.starts_with("https://")) {
        invalid("DonBot API base URL must use HTTPS");
        return;
    }
    if (has_ascii_space_or_control(value) || value.contains('@') || value.contains('?') ||
        value.contains('#') || value.contains('\\') || value.find("/../", 8) != std::string::npos ||
        value.ends_with("/..") || value.find("/./", 8) != std::string::npos ||
        value.ends_with("/.")) {
        invalid("DonBot API base URL contains a forbidden component");
        return;
    }

    constexpr std::size_t scheme_size = 8;
    const auto host_end = value.find('/', scheme_size);
    const auto host = host_end == std::string_view::npos
                          ? value.substr(scheme_size)
                          : value.substr(scheme_size, host_end - scheme_size);
    if (host.empty()) {
        invalid("DonBot API base URL must include a host");
    }
}

void validate_guild_id(std::string_view value, std::vector<SettingsValidationError>& errors) {
    if (value.empty()) {
        return;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    const bool valid =
        value.size() <= max_guild_id_bytes && result.ec == std::errc{} &&
        result.ptr == value.data() + value.size() && parsed > 0 &&
        parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!valid) {
        add_error(errors, SettingsValidationErrorCode::InvalidGuildId, "donbot.selected_guild_id",
                  "DonBot guild ID must be a positive decimal Discord snowflake");
    }
}

void validate_channel_id(std::string_view value, std::vector<SettingsValidationError>& errors) {
    if (value.empty()) {
        return;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    const bool valid =
        value.size() <= max_guild_id_bytes && result.ec == std::errc{} &&
        result.ptr == value.data() + value.size() && parsed > 0 &&
        parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!valid) {
        add_error(errors, SettingsValidationErrorCode::InvalidChannelId,
                  "donbot.selected_discord_channel_id",
                  "DonBot Discord channel ID must be a positive decimal Discord snowflake");
    }
}

void validate_twitch_client_id(std::string_view value,
                               std::vector<SettingsValidationError>& errors) {
    if (value.empty()) {
        return;
    }
    if (value.size() > max_twitch_client_id_bytes ||
        !std::ranges::all_of(value, [](char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        })) {
        add_error(errors, SettingsValidationErrorCode::InvalidTwitchClientId, "twitch.client_id",
                  "Twitch application client ID must contain only lowercase letters and digits");
    }
}

void validate_template(std::string_view value, std::vector<SettingsValidationError>& errors) {
    constexpr std::string_view field_name = "twitch.message_template";
    const auto parsed = application::TwitchMessageTemplate::parse(value);
    if (parsed) {
        return;
    }
    auto code = SettingsValidationErrorCode::InvalidTemplate;
    switch (parsed.error().code) {
    case application::TwitchTemplateErrorCode::EmptyTemplate:
        code = SettingsValidationErrorCode::EmptyValue;
        break;
    case application::TwitchTemplateErrorCode::InvalidUtf8:
        code = SettingsValidationErrorCode::InvalidUtf8;
        break;
    case application::TwitchTemplateErrorCode::TemplateTooLong:
        code = SettingsValidationErrorCode::ValueTooLong;
        break;
    case application::TwitchTemplateErrorCode::MissingPermalinkField:
        code = SettingsValidationErrorCode::MissingUrlPlaceholder;
        break;
    case application::TwitchTemplateErrorCode::ControlCharacter:
    case application::TwitchTemplateErrorCode::InvalidSyntax:
    case application::TwitchTemplateErrorCode::UnknownField:
    case application::TwitchTemplateErrorCode::InvalidFieldValue:
    case application::TwitchTemplateErrorCode::EmptyMessage:
    case application::TwitchTemplateErrorCode::MessageTooLong:
    case application::TwitchTemplateErrorCode::AllocationFailed:
        break;
    }
    add_error(errors, code, std::string{field_name}, parsed.error().message);
}

} // namespace

Settings make_default_settings(std::string log_directory) {
    Settings settings;
    settings.general.log_directory = std::move(log_directory);
    return settings;
}

std::vector<SettingsValidationError> validate_settings(const Settings& settings) {
    std::vector<SettingsValidationError> errors;
    if (settings.schema_version != current_settings_schema_version) {
        add_error(errors, SettingsValidationErrorCode::UnsupportedSchemaVersion, "schema_version",
                  "Settings schema version is not supported");
    }

    validate_log_directory(settings.general.log_directory, errors);
    validate_range(settings.general.poll_interval_ms, 250, 60'000, "general.poll_interval_ms",
                   errors);
    validate_range(settings.general.stability_observations, 2, 10, "general.stability_observations",
                   errors);
    validate_range(settings.general.recent_log_limit, 1, 500, "general.recent_log_limit", errors);
    validate_range(settings.general.parser_queue_capacity, 1, 64, "general.parser_queue_capacity",
                   errors);
    validate_range(settings.general.parallel_uploads_per_provider, 1, 32,
                   "general.parallel_uploads_per_provider", errors);
    validate_range(settings.general.max_candidates, 1, 10'000, "general.max_candidates", errors);
    validate_donbot_url(settings.donbot.api_base_url, errors);
    validate_guild_id(settings.donbot.selected_guild_id, errors);
    validate_channel_id(settings.donbot.selected_discord_channel_id, errors);
    validate_twitch_client_id(settings.twitch.client_id, errors);
    validate_template(settings.twitch.message_template, errors);

    if (settings.twitch.enabled && !settings.dps_report.enabled) {
        add_error(errors, SettingsValidationErrorCode::TwitchRequiresDpsReport, "twitch.enabled",
                  "Twitch posting requires dps.report to be enabled");
    }
    if (settings.donbot.enabled && settings.donbot.selected_guild_id.empty()) {
        add_error(errors, SettingsValidationErrorCode::EmptyValue, "donbot.selected_guild_id",
                  "Enabling DonBot requires a verified guild selection");
    }
    if (settings.donbot.discord_delivery_enabled && !settings.donbot.enabled) {
        add_error(errors, SettingsValidationErrorCode::DonBotDiscordDeliveryRequiresUpload,
                  "donbot.discord_delivery_enabled",
                  "DonBot Discord delivery requires DonBot uploads to be enabled");
    }
    if (settings.twitch.enabled && !settings.twitch.post_success && !settings.twitch.post_failure) {
        add_error(errors, SettingsValidationErrorCode::TwitchPostingDisabled, "twitch.enabled",
                  "Twitch posting requires at least one encounter result policy");
    }
    return errors;
}

domain::ProviderSelection enabled_provider_selection(const Settings& settings) noexcept {
    domain::ProviderSelection selection{};
    selection[domain::provider_index(domain::Provider::DpsReport)] = settings.dps_report.enabled;
    selection[domain::provider_index(domain::Provider::Wingman)] = settings.wingman.enabled;
    selection[domain::provider_index(domain::Provider::DonBot)] = settings.donbot.enabled;
    selection[domain::provider_index(domain::Provider::Twitch)] = settings.twitch.enabled;
    return selection;
}

} // namespace manny_uploader::config
