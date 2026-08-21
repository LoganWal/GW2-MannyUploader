#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace manny_uploader::config {

inline constexpr std::uint32_t current_settings_schema_version = 1;
inline constexpr std::string_view default_donbot_api_base = "https://donbot-api.walmslo.com";
inline constexpr std::string_view default_twitch_message_template =
    "{encounter}{mode_suffix} — {result}: {url}";

struct GeneralSettings {
    std::string log_directory;
    bool watch_subdirectories{true};
    bool window_visible{true};
    std::uint32_t poll_interval_ms{1000};
    std::uint32_t stability_observations{2};
    std::uint32_t recent_log_limit{50};
    std::uint32_t parser_queue_capacity{8};
    std::uint32_t max_candidates{4096};

    [[nodiscard]] friend bool operator==(const GeneralSettings&,
                                         const GeneralSettings&) noexcept = default;
};

struct DpsReportSettings {
    bool enabled{true};

    [[nodiscard]] friend bool operator==(DpsReportSettings, DpsReportSettings) noexcept = default;
};

struct WingmanSettings {
    bool enabled{true};

    [[nodiscard]] friend bool operator==(WingmanSettings, WingmanSettings) noexcept = default;
};

struct DonBotSettings {
    bool enabled{};
    std::string api_base_url{default_donbot_api_base};
    std::string selected_guild_id;

    [[nodiscard]] friend bool operator==(const DonBotSettings&,
                                         const DonBotSettings&) noexcept = default;
};

struct TwitchSettings {
    bool enabled{};
    std::string message_template{default_twitch_message_template};
    bool post_success{true};
    bool post_failure{true};

    [[nodiscard]] friend bool operator==(const TwitchSettings&,
                                         const TwitchSettings&) noexcept = default;
};

struct Settings {
    std::uint32_t schema_version{current_settings_schema_version};
    GeneralSettings general;
    DpsReportSettings dps_report;
    WingmanSettings wingman;
    DonBotSettings donbot;
    TwitchSettings twitch;

    [[nodiscard]] friend bool operator==(const Settings&, const Settings&) noexcept = default;
};

enum class SettingsValidationErrorCode : std::uint8_t {
    UnsupportedSchemaVersion,
    EmptyValue,
    InvalidPath,
    InvalidUtf8,
    ValueTooLong,
    OutOfRange,
    InvalidUrl,
    InvalidGuildId,
    TwitchRequiresDpsReport,
    TwitchPostingDisabled,
    InvalidTemplate,
    MissingUrlPlaceholder,
};

struct SettingsValidationError {
    SettingsValidationErrorCode code;
    std::string field;
    std::string message;
};

[[nodiscard]] Settings make_default_settings(std::string log_directory);
[[nodiscard]] std::vector<SettingsValidationError> validate_settings(const Settings& settings);
[[nodiscard]] domain::ProviderSelection
enabled_provider_selection(const Settings& settings) noexcept;

} // namespace manny_uploader::config
