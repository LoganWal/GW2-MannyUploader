#include "manny_uploader/config/settings.hpp"
#include "support/test_suite.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace manny_uploader::test {
namespace {

using config::Settings;
using config::SettingsValidationError;
using config::SettingsValidationErrorCode;

[[nodiscard]] Settings valid_settings() {
    return config::make_default_settings("C:/Users/Streamer/Documents/Guild Wars 2/arcdps.cbtlogs");
}

[[nodiscard]] bool has_error(const std::vector<SettingsValidationError>& errors,
                             SettingsValidationErrorCode code, std::string_view field) {
    return std::ranges::any_of(errors, [code, field](const auto& error) {
        return error.code == code && error.field == field;
    });
}

void default_and_selection_tests(TestSuite& suite) {
    const auto settings = valid_settings();
    MANNY_CHECK(suite, settings.schema_version == 1);
    MANNY_CHECK(suite, settings.general.watch_subdirectories);
    MANNY_CHECK(suite, settings.general.window_visible);
    MANNY_CHECK(suite, settings.general.poll_interval_ms == 1000);
    MANNY_CHECK(suite, settings.general.stability_observations == 2);
    MANNY_CHECK(suite, settings.general.recent_log_limit == 100);
    MANNY_CHECK(suite, settings.general.parser_queue_capacity == 8);
    MANNY_CHECK(suite, settings.general.parallel_uploads_per_provider == 5);
    MANNY_CHECK(suite, settings.general.max_candidates == 4096);
    MANNY_CHECK(suite, settings.dps_report.enabled);
    MANNY_CHECK(suite, !settings.dps_report.detailed_wvw);
    MANNY_CHECK(suite, settings.wingman.enabled);
    MANNY_CHECK(suite, !settings.donbot.enabled);
    MANNY_CHECK(suite, settings.donbot.api_base_url == "https://donbot-api.walmslo.com");
    MANNY_CHECK(suite, !settings.donbot.discord_delivery_enabled);
    MANNY_CHECK(suite, !settings.donbot.discord_channel_override_explicit);
    MANNY_CHECK(suite, settings.donbot.selected_discord_channel_id.empty());
    MANNY_CHECK(suite, !settings.twitch.enabled);
    MANNY_CHECK(suite, settings.twitch.client_id.empty());
    MANNY_CHECK(suite, settings.twitch.message_template.find("{url}") != std::string::npos);
    MANNY_CHECK(suite, config::validate_settings(settings).empty());

    const auto selection = config::enabled_provider_selection(settings);
    MANNY_CHECK(suite, selection[domain::provider_index(domain::Provider::DpsReport)]);
    MANNY_CHECK(suite, selection[domain::provider_index(domain::Provider::Wingman)]);
    MANNY_CHECK(suite, !selection[domain::provider_index(domain::Provider::DonBot)]);
    MANNY_CHECK(suite, !selection[domain::provider_index(domain::Provider::Twitch)]);

    auto all = settings;
    all.donbot.enabled = true;
    all.twitch.enabled = true;
    const auto all_selection = config::enabled_provider_selection(all);
    MANNY_CHECK(suite, all_selection[domain::provider_index(domain::Provider::Wingman)]);
    MANNY_CHECK(suite, all_selection[domain::provider_index(domain::Provider::DonBot)]);
    MANNY_CHECK(suite, all_selection[domain::provider_index(domain::Provider::Twitch)]);
}

void schema_and_path_tests(TestSuite& suite) {
    auto settings = valid_settings();
    settings.schema_version = 2;
    auto errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::UnsupportedSchemaVersion,
                                 "schema_version"));

    settings = valid_settings();
    settings.general.log_directory.clear();
    errors = config::validate_settings(settings);
    MANNY_CHECK(
        suite, has_error(errors, SettingsValidationErrorCode::EmptyValue, "general.log_directory"));

    settings = valid_settings();
    settings.general.log_directory = std::string(4097, 'x');
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::ValueTooLong,
                                 "general.log_directory"));

    settings = valid_settings();
    settings.general.log_directory = "logs\ninvalid";
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::InvalidPath,
                                 "general.log_directory"));

    settings = valid_settings();
    settings.general.log_directory = "C:/Guild Wars 2/strÃ¶m";
    MANNY_CHECK(suite, config::validate_settings(settings).empty());

    settings.general.log_directory = std::string{"bad\xc0\x80", 5};
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::InvalidUtf8,
                                 "general.log_directory"));
}

void numeric_range_tests(TestSuite& suite) {
    auto settings = valid_settings();
    settings.general.poll_interval_ms = 249;
    auto errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.poll_interval_ms"));
    settings.general.poll_interval_ms = 60'001;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.poll_interval_ms"));

    settings = valid_settings();
    settings.general.stability_observations = 1;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.stability_observations"));
    settings.general.stability_observations = 11;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.stability_observations"));

    settings = valid_settings();
    settings.general.recent_log_limit = 0;
    settings.general.parser_queue_capacity = 65;
    settings.general.parallel_uploads_per_provider = 0;
    settings.general.max_candidates = 10'001;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.recent_log_limit"));
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.parser_queue_capacity"));
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.parallel_uploads_per_provider"));
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::OutOfRange,
                                 "general.max_candidates"));

    settings = valid_settings();
    settings.general.poll_interval_ms = 250;
    settings.general.stability_observations = 10;
    settings.general.recent_log_limit = 500;
    settings.general.parser_queue_capacity = 64;
    settings.general.parallel_uploads_per_provider = 32;
    settings.general.max_candidates = 10'000;
    MANNY_CHECK(suite, config::validate_settings(settings).empty());
}

void donbot_validation_tests(TestSuite& suite) {
    auto settings = valid_settings();
    settings.donbot.api_base_url = "https://donbot.example/api/";
    settings.donbot.selected_guild_id = "123456789012345678";
    MANNY_CHECK(suite, config::validate_settings(settings).empty());

    for (const auto& invalid_url : {
             std::string{"http://donbot.example"},
             std::string{"https://"},
             std::string{"https://user@donbot.example"},
             std::string{"https://donbot.example?mode=test"},
             std::string{"https://donbot.example/#fragment"},
             std::string{"https://don bot.example"},
             std::string{"https://donbot.example/../other"},
             std::string{"https://donbot.example\\other"},
         }) {
        settings = valid_settings();
        settings.donbot.api_base_url = invalid_url;
        const auto errors = config::validate_settings(settings);
        MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::InvalidUrl,
                                     "donbot.api_base_url"));
    }

    settings = valid_settings();
    settings.donbot.selected_guild_id = "not_a_guild";
    auto errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::InvalidGuildId,
                                 "donbot.selected_guild_id"));

    settings.donbot.selected_guild_id = "9223372036854775808";
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::InvalidGuildId,
                                 "donbot.selected_guild_id"));

    settings = valid_settings();
    settings.donbot.enabled = true;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::EmptyValue,
                                 "donbot.selected_guild_id"));

    settings = valid_settings();
    settings.donbot.selected_discord_channel_id = "not-a-channel";
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::InvalidChannelId,
                                 "donbot.selected_discord_channel_id"));

    settings = valid_settings();
    settings.donbot.discord_delivery_enabled = true;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite,
                has_error(errors, SettingsValidationErrorCode::DonBotDiscordDeliveryRequiresUpload,
                          "donbot.discord_delivery_enabled"));

    settings = valid_settings();
    settings.donbot.enabled = true;
    settings.donbot.selected_guild_id = "123";
    settings.donbot.discord_delivery_enabled = true;
    settings.donbot.discord_channel_override_explicit = true;
    settings.donbot.selected_discord_channel_id = "223456789012345678";
    MANNY_CHECK(suite, config::validate_settings(settings).empty());
}

void twitch_validation_tests(TestSuite& suite) {
    auto settings = valid_settings();
    settings.twitch.client_id = "abc123publicclient";
    settings.twitch.message_template =
        "{{log}} {encounter} {mode} {mode_suffix} {result} {boss_id}: {url}";
    MANNY_CHECK(suite, config::validate_settings(settings).empty());

    settings = valid_settings();
    settings.twitch.client_id = "BAD-CLIENT-ID";
    auto client_id_errors = config::validate_settings(settings);
    MANNY_CHECK(suite,
                has_error(client_id_errors, SettingsValidationErrorCode::InvalidTwitchClientId,
                          "twitch.client_id"));

    for (const auto& invalid_template : {
             std::string{"{unknown}: {url}"},
             std::string{"{} {url}"},
             std::string{"{encounter {url}"},
             std::string{"{encounter}} {url}"},
             std::string{"{encounter}"},
         }) {
        settings = valid_settings();
        settings.twitch.message_template = invalid_template;
        const auto errors = config::validate_settings(settings);
        MANNY_CHECK(suite, !errors.empty());
    }

    settings = valid_settings();
    settings.twitch.message_template = std::string(501, 'x') + "{url}";
    auto errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::ValueTooLong,
                                 "twitch.message_template"));

    settings = valid_settings();
    settings.twitch.enabled = true;
    settings.dps_report.enabled = false;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::TwitchRequiresDpsReport,
                                 "twitch.enabled"));

    settings = valid_settings();
    settings.twitch.enabled = true;
    settings.twitch.post_success = false;
    settings.twitch.post_failure = false;
    errors = config::validate_settings(settings);
    MANNY_CHECK(suite, has_error(errors, SettingsValidationErrorCode::TwitchPostingDisabled,
                                 "twitch.enabled"));
}

} // namespace

void run_settings_tests(TestSuite& suite) {
    default_and_selection_tests(suite);
    schema_and_path_tests(suite);
    numeric_range_tests(suite);
    donbot_validation_tests(suite);
    twitch_validation_tests(suite);
}

} // namespace manny_uploader::test
