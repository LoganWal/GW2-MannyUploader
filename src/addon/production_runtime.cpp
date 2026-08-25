#include "production_runtime.hpp"

#include "manny_uploader/application/application_pump.hpp"
#include "manny_uploader/application/configuration_service.hpp"
#include "manny_uploader/application/donbot_configuration_controller.hpp"
#include "manny_uploader/application/log_ingestion_coordinator.hpp"
#include "manny_uploader/application/log_selection.hpp"
#include "manny_uploader/application/nexus_options_controller.hpp"
#include "manny_uploader/application/recent_log_actions_controller.hpp"
#include "manny_uploader/application/twitch_authentication_controller.hpp"
#include "manny_uploader/application/twitch_session_owner.hpp"
#include "manny_uploader/application/upload_coordinator.hpp"
#include "manny_uploader/config/protected_file_secret_store.hpp"
#include "manny_uploader/config/secret_protector.hpp"
#include "manny_uploader/config/settings_store.hpp"
#include "manny_uploader/config/upload_history_store.hpp"
#include "manny_uploader/evtc/metadata_parser_worker.hpp"
#include "manny_uploader/evtc/zevtc_archive.hpp"
#include "manny_uploader/filesystem/change_notifying_log_candidate_source.hpp"
#include "manny_uploader/http/curl_http_client.hpp"
#include "manny_uploader/ports/clock.hpp"
#include "manny_uploader/ports/external_action_launcher.hpp"
#include "manny_uploader/providers/donbot_client.hpp"
#include "manny_uploader/providers/donbot_provider_worker.hpp"
#include "manny_uploader/providers/donbot_verification_worker.hpp"
#include "manny_uploader/providers/dps_report_client.hpp"
#include "manny_uploader/providers/dps_report_provider_worker.hpp"
#include "manny_uploader/providers/twitch_authentication_worker.hpp"
#include "manny_uploader/providers/twitch_client.hpp"
#include "manny_uploader/providers/twitch_provider_worker.hpp"
#include "manny_uploader/providers/twitch_test_message_worker.hpp"
#include "manny_uploader/providers/wingman_client.hpp"
#include "manny_uploader/providers/wingman_provider_worker.hpp"
#include "manny_uploader/support/secret_value.hpp"
#include "manny_uploader/ui/nexus_options_model.hpp"

#include <imgui.h>
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manny_uploader::addon {
namespace {

using namespace std::chrono_literals;

inline constexpr char window_name[] = "GW2 Manny Uploader";
inline constexpr char settings_filename[] = "settings.json";
inline constexpr char upload_history_filename[] = "upload-history.json";
inline constexpr char secrets_directory_name[] = "secrets";
inline constexpr char default_log_directory_name[] = "arcdps.cbtlogs";
inline constexpr std::size_t provider_queue_capacity = 500;
inline constexpr std::size_t accepted_log_dedupe_capacity = 10'000;

[[nodiscard]] std::string effective_twitch_client_id(const config::Settings& settings) {
    if (!settings.twitch.client_id.empty()) {
        return settings.twitch.client_id;
    }
#if defined(MANNY_TWITCH_CLIENT_ID)
    return MANNY_TWITCH_CLIENT_ID;
#else
    return {};
#endif
}

[[nodiscard]] std::filesystem::file_time_type
local_day_started_at(std::chrono::system_clock::time_point now) noexcept {
    const auto timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (localtime_s(&local, &timestamp) != 0) {
        return application::file_time_from_system(std::chrono::floor<std::chrono::days>(now));
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const auto midnight = std::mktime(&local);
    if (midnight == static_cast<std::time_t>(-1)) {
        return application::file_time_from_system(std::chrono::floor<std::chrono::days>(now));
    }
    return application::file_time_from_system(std::chrono::system_clock::from_time_t(midnight));
}

class SystemClock final : public ports::IClock {
  public:
    [[nodiscard]] std::chrono::system_clock::time_point system_now() const noexcept override {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

[[nodiscard]] ports::SecretStoreError
unavailable_secret_error(ports::SecretId id,
                         std::string message = "Protected credential storage is unavailable") {
    return ports::SecretStoreError{
        .code = ports::SecretStoreErrorCode::UnsupportedEnvironment,
        .id = id,
        .message = std::move(message),
        .system_error = std::nullopt,
    };
}

class UnavailableSecretStore final : public ports::ISecretStore {
  public:
    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(ports::SecretId id) const override {
        return std::unexpected(unavailable_secret_error(id));
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(ports::SecretId id, const support::SecretValue&) override {
        return std::unexpected(unavailable_secret_error(id));
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(ports::SecretId id) override {
        return std::unexpected(unavailable_secret_error(id));
    }
};

[[nodiscard]] ports::ExternalActionError launch_error(std::string message) {
    return ports::ExternalActionError{
        .code = ports::ExternalActionErrorCode::LaunchFailed,
        .message = std::move(message),
    };
}

class WindowsExternalActionLauncher final : public ports::IExternalActionLauncher {
  public:
    [[nodiscard]] std::expected<void, ports::ExternalActionError>
    open_url(std::string_view url) override {
        const std::wstring target{url.begin(), url.end()};
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr,
                                                    nullptr, SW_SHOWNORMAL)) <= 32) {
            return std::unexpected(launch_error("Windows could not open the dps.report link"));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ports::ExternalActionError>
    open_directory(const std::filesystem::path& directory) override {
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr,
                                                    nullptr, SW_SHOWNORMAL)) <= 32) {
            return std::unexpected(launch_error("Windows could not open the log directory"));
        }
        return {};
    }
};

struct RuntimeComponents {
    std::unique_ptr<SystemClock> clock;
    std::unique_ptr<ports::IHttpClient> http;
    std::unique_ptr<application::ConfigurationService> configuration;
    std::unique_ptr<UnavailableSecretStore> unavailable_secrets;
    ports::ISecretStore* provider_secrets{};
    bool persistent_secrets_available{};
    bool wine_secret_compatibility_mode{};
    std::filesystem::file_time_type session_started_at;
    std::string startup_diagnostic;

    std::unique_ptr<providers::DpsReportClient> dps_report_client;
    std::unique_ptr<providers::WingmanClient> wingman_client;
    std::unique_ptr<providers::DonBotClient> donbot_client;
    std::unique_ptr<providers::TwitchClient> twitch_client;
    std::unique_ptr<config::UploadHistoryStore> upload_history_store;

    std::unique_ptr<application::TwitchSessionOwner> twitch_session_owner;
    std::unique_ptr<providers::DpsReportProviderWorker> dps_report_worker;
    std::unique_ptr<providers::WingmanProviderWorker> wingman_worker;
    std::unique_ptr<providers::DonBotProviderWorker> donbot_worker;
    std::unique_ptr<providers::TwitchProviderWorker> twitch_worker;
    std::unique_ptr<providers::DonBotVerificationWorker> donbot_verification_worker;
    std::unique_ptr<providers::TwitchAuthenticationWorker> twitch_authentication_worker;
    std::unique_ptr<providers::TwitchTestMessageWorker> twitch_test_message_worker;

    std::unique_ptr<application::DonBotConfigurationController> donbot_controller;
    std::unique_ptr<application::TwitchAuthenticationController> twitch_controller;
    std::unique_ptr<application::NexusOptionsController> options_controller;

    std::unique_ptr<evtc::ZevtcMetadataReader> metadata_reader;
    std::unique_ptr<evtc::MetadataParserWorker> metadata_worker;
    std::unique_ptr<filesystem::ChangeNotifyingLogCandidateSource> candidate_source;
    std::unique_ptr<application::UploadCoordinator> upload_coordinator;
    std::unique_ptr<WindowsExternalActionLauncher> external_action_launcher;
    std::unique_ptr<application::RecentLogActionsController> recent_log_actions;
    std::unique_ptr<application::LogIngestionCoordinator> ingestion_coordinator;
    std::unique_ptr<application::ApplicationPump> application_pump;
};

struct ProviderCell {
    domain::ProviderState state{domain::ProviderState::Disabled};
    std::string status;
    std::string detail;
    bool retry_available{};
};

struct RecentLogRow {
    std::uint64_t id{};
    std::string detected_at;
    std::string filename;
    std::string encounter;
    std::string dps_report_url;
    std::optional<std::uint64_t> donbot_fight_log_id;
    std::optional<bool> encounter_success;
    bool report_available{};
    bool wingman_report_available{};
    bool directory_available{};
    bool reupload_available{};
    bool rechat_available{};
    std::array<ProviderCell, domain::provider_count> providers;
};

struct RuntimeSnapshot {
    application::NexusOptionsSnapshot options_snapshot;
    application::RecentLogActionsSnapshot recent_log_actions;
    ui::NexusOptionsModel options_model;
    domain::ProviderSelection enabled_providers;
    std::vector<RecentLogRow> recent_logs;
    std::string dps_report_clipboard_text;
    std::string donbot_aggregate_url;
    std::string donbot_destination_status;
    std::size_t dps_report_url_count{};
    std::size_t donbot_fight_count{};
    std::string runtime_diagnostic;
    bool log_root_available{};
    application::LogSelectionMode log_selection_mode{application::LogSelectionMode::New};
    application::LogSelectionWindow log_selection_window;
    bool wine_secret_compatibility_mode{};
    bool twitch_application_configured{};
    std::uint64_t revision{};
};

struct GeneralSettingsApplyReport {
    bool poll_now{};
    bool root_changed{};
};

[[nodiscard]] AddonRuntimeError runtime_error(std::string message) {
    return AddonRuntimeError{
        .code = AddonRuntimeErrorCode::InitializationFailed,
        .message = std::move(message),
    };
}

[[nodiscard]] ports::SecretStoreError
map_protector_error(const config::SecretProtectionError& error) {
    return ports::SecretStoreError{
        .code = error.code == config::SecretProtectionErrorCode::UnsupportedEnvironment
                    ? ports::SecretStoreErrorCode::UnsupportedEnvironment
                    : ports::SecretStoreErrorCode::ProtectionFailed,
        .id = std::nullopt,
        .message = error.message,
        .system_error = error.system_error,
    };
}

[[nodiscard]] std::expected<std::unique_ptr<RuntimeComponents>, AddonRuntimeError>
create_components(const AddonPaths& paths) {
    if (paths.game_directory.empty() || paths.addon_directory.empty()) {
        return std::unexpected(runtime_error("Nexus runtime directories are empty"));
    }

    try {
        std::error_code directory_error;
        std::filesystem::create_directories(paths.addon_directory, directory_error);
        if (directory_error) {
            return std::unexpected(runtime_error("Unable to create the addon data directory"));
        }

        auto result = std::make_unique<RuntimeComponents>();
        result->clock = std::make_unique<SystemClock>();
        result->session_started_at =
            application::file_time_from_system(result->clock->system_now());
        result->wine_secret_compatibility_mode = config::is_wine_environment();

        auto http = http::make_curl_http_client();
        if (!http) {
            return std::unexpected(runtime_error("Unable to initialize the HTTP transport"));
        }
        result->http = std::move(*http);

        const auto default_log_directory = paths.game_directory / default_log_directory_name;
        auto settings = config::SettingsStore::create(
            paths.addon_directory / settings_filename,
            config::make_default_settings(default_log_directory.string()));
        if (!settings) {
            return std::unexpected(runtime_error("Unable to initialize ordinary settings"));
        }
        std::unique_ptr<ports::ISettingsStore> settings_port =
            std::make_unique<config::SettingsStore>(std::move(*settings));

        auto protector = config::make_dpapi_secret_protector();
        std::expected<application::ConfigurationService, application::ConfigurationError>
            configured = [&]() {
                if (!protector) {
                    return application::ConfigurationService::create_without_secret_storage(
                        std::move(settings_port), map_protector_error(protector.error()));
                }
                auto secret_store = config::ProtectedFileSecretStore::create(
                    paths.addon_directory / secrets_directory_name, std::move(*protector));
                if (!secret_store) {
                    return application::ConfigurationService::create_without_secret_storage(
                        std::move(settings_port), secret_store.error());
                }
                auto owned_secret_store =
                    std::make_unique<config::ProtectedFileSecretStore>(std::move(*secret_store));
                result->provider_secrets = owned_secret_store.get();
                result->persistent_secrets_available = true;
                return application::ConfigurationService::create(std::move(settings_port),
                                                                 std::move(owned_secret_store));
            }();
        if (!configured) {
            return std::unexpected(runtime_error("Unable to load the uploader configuration"));
        }
        result->configuration =
            std::make_unique<application::ConfigurationService>(std::move(*configured));
        if (!result->persistent_secrets_available) {
            result->unavailable_secrets = std::make_unique<UnavailableSecretStore>();
            result->provider_secrets = result->unavailable_secrets.get();
        }

        const auto initial_settings = result->configuration->snapshot().settings;
        auto history =
            config::UploadHistoryStore::create(paths.addon_directory / upload_history_filename);
        if (!history) {
            return std::unexpected(runtime_error("Unable to initialize persistent upload history"));
        }
        result->startup_diagnostic = history->recovery_diagnostic();
        result->upload_history_store =
            std::make_unique<config::UploadHistoryStore>(std::move(*history));

        result->dps_report_client = std::make_unique<providers::DpsReportClient>(*result->http);
        result->wingman_client = std::make_unique<providers::WingmanClient>(*result->http);
        result->donbot_client = std::make_unique<providers::DonBotClient>(*result->http);

        auto twitch_client_id = effective_twitch_client_id(initial_settings);
        if (twitch_client_id.empty()) {
            result->twitch_client = std::make_unique<providers::TwitchClient>(
                providers::TwitchClient::create_unconfigured(*result->http));
        } else {
            auto twitch =
                providers::TwitchClient::create(*result->http, std::move(twitch_client_id));
            if (!twitch) {
                return std::unexpected(runtime_error("The Twitch application ID is invalid"));
            }
            result->twitch_client = std::make_unique<providers::TwitchClient>(std::move(*twitch));
        }
        result->twitch_session_owner = std::make_unique<application::TwitchSessionOwner>(
            *result->configuration, *result->twitch_client, *result->clock);

        auto dps_worker = providers::DpsReportProviderWorker::create(
            *result->dps_report_client,
            result->persistent_secrets_available ? result->provider_secrets : nullptr,
            provider_queue_capacity, initial_settings.general.parallel_uploads_per_provider);
        if (!dps_worker) {
            return std::unexpected(runtime_error("Unable to start the dps.report worker"));
        }
        result->dps_report_worker = std::move(*dps_worker);

        auto wingman_worker = providers::WingmanProviderWorker::create(
            *result->wingman_client, provider_queue_capacity,
            initial_settings.general.parallel_uploads_per_provider);
        if (!wingman_worker) {
            return std::unexpected(runtime_error("Unable to start the GW2Wingman worker"));
        }
        result->wingman_worker = std::move(*wingman_worker);

        auto donbot_worker = providers::DonBotProviderWorker::create(
            *result->donbot_client, *result->provider_secrets,
            providers::DonBotProviderConfig{
                .api_base_url = initial_settings.donbot.api_base_url,
                .guild_id = initial_settings.donbot.selected_guild_id,
            },
            provider_queue_capacity, initial_settings.general.parallel_uploads_per_provider);
        if (!donbot_worker) {
            return std::unexpected(runtime_error("Unable to start the DonBot worker"));
        }
        result->donbot_worker = std::move(*donbot_worker);

        auto twitch_worker = providers::TwitchProviderWorker::create(
            *result->twitch_client, *result->twitch_session_owner,
            providers::TwitchProviderConfig{
                .message_template = initial_settings.twitch.message_template,
                .post_success = initial_settings.twitch.post_success,
                .post_failure = initial_settings.twitch.post_failure,
            },
            provider_queue_capacity, initial_settings.general.parallel_uploads_per_provider);
        if (!twitch_worker) {
            return std::unexpected(runtime_error("Unable to start the Twitch delivery worker"));
        }
        result->twitch_worker = std::move(*twitch_worker);

        auto donbot_verifier = providers::DonBotVerificationWorker::create(*result->donbot_client);
        if (!donbot_verifier) {
            return std::unexpected(runtime_error("Unable to start DonBot verification"));
        }
        result->donbot_verification_worker = std::move(*donbot_verifier);

        auto twitch_authentication =
            providers::TwitchAuthenticationWorker::create(*result->twitch_client);
        if (!twitch_authentication) {
            return std::unexpected(runtime_error("Unable to start Twitch authentication"));
        }
        result->twitch_authentication_worker = std::move(*twitch_authentication);

        auto twitch_test = providers::TwitchTestMessageWorker::create(
            *result->twitch_client, *result->twitch_session_owner);
        if (!twitch_test) {
            return std::unexpected(runtime_error("Unable to start Twitch test delivery"));
        }
        result->twitch_test_message_worker = std::move(*twitch_test);

        auto donbot_controller = application::DonBotConfigurationController::create(
            *result->configuration, *result->donbot_verification_worker);
        if (!donbot_controller) {
            return std::unexpected(runtime_error("Unable to initialize DonBot configuration"));
        }
        result->donbot_controller = std::make_unique<application::DonBotConfigurationController>(
            std::move(*donbot_controller));

        auto twitch_controller = application::TwitchAuthenticationController::create(
            *result->configuration, *result->twitch_authentication_worker,
            *result->twitch_session_owner, *result->clock);
        if (!twitch_controller) {
            return std::unexpected(runtime_error("Unable to initialize Twitch authentication"));
        }
        result->twitch_controller = std::make_unique<application::TwitchAuthenticationController>(
            std::move(*twitch_controller));

        auto options = application::NexusOptionsController::create(
            *result->configuration, *result->donbot_controller, *result->twitch_controller,
            *result->twitch_test_message_worker);
        if (!options) {
            return std::unexpected(runtime_error("Unable to initialize Nexus options"));
        }
        result->options_controller =
            std::make_unique<application::NexusOptionsController>(std::move(*options));

        auto metadata_reader = evtc::ZevtcMetadataReader::create();
        if (!metadata_reader) {
            return std::unexpected(runtime_error("Unable to initialize the EVTC archive reader"));
        }
        result->metadata_reader =
            std::make_unique<evtc::ZevtcMetadataReader>(std::move(*metadata_reader));

        auto metadata_worker = evtc::MetadataParserWorker::create(
            *result->metadata_reader, initial_settings.general.parser_queue_capacity);
        if (!metadata_worker) {
            return std::unexpected(runtime_error("Unable to start the EVTC metadata worker"));
        }
        result->metadata_worker = std::move(*metadata_worker);

        auto candidate_source = filesystem::ChangeNotifyingLogCandidateSource::create(
            std::filesystem::path{initial_settings.general.log_directory},
            initial_settings.general.watch_subdirectories, initial_settings.general.max_candidates,
            filesystem::make_windows_directory_change_monitor(), 3, result->session_started_at);
        if (!candidate_source) {
            return std::unexpected(runtime_error("Unable to initialize log-directory monitoring"));
        }
        result->candidate_source = std::make_unique<filesystem::ChangeNotifyingLogCandidateSource>(
            std::move(*candidate_source));

        std::array<ports::IUploadProvider*, domain::provider_count> provider_ports{
            result->dps_report_worker.get(),
            result->wingman_worker.get(),
            result->donbot_worker.get(),
            result->twitch_worker.get(),
        };
        auto uploads = application::UploadCoordinator::create(
            *result->clock, provider_ports, initial_settings.general.recent_log_limit);
        if (!uploads) {
            return std::unexpected(runtime_error("Unable to initialize upload coordination"));
        }
        result->upload_coordinator =
            std::make_unique<application::UploadCoordinator>(std::move(*uploads));
        if (auto restored = result->upload_coordinator->restore_history(
                result->upload_history_store->records());
            !restored) {
            return std::unexpected(runtime_error("Unable to restore persistent upload history"));
        }
        result->external_action_launcher = std::make_unique<WindowsExternalActionLauncher>();
        auto recent_log_actions = application::RecentLogActionsController::create(
            *result->upload_coordinator, *result->external_action_launcher);
        if (!recent_log_actions) {
            return std::unexpected(runtime_error("Unable to initialize recent-log actions"));
        }
        result->recent_log_actions = std::move(*recent_log_actions);
        result->ingestion_coordinator = std::make_unique<application::LogIngestionCoordinator>(
            *result->upload_coordinator, *result->metadata_worker);

        auto pump = application::ApplicationPump::create(
            *result->candidate_source, *result->metadata_worker, *result->ingestion_coordinator,
            application::ApplicationPumpConfig{
                .required_matching_observations = initial_settings.general.stability_observations,
                .dedupe_capacity = accepted_log_dedupe_capacity,
                .max_metadata_results_per_tick = 8,
                .max_upload_results_per_tick = 8,
            });
        if (!pump) {
            return std::unexpected(runtime_error("Unable to initialize the application pump"));
        }
        result->application_pump = std::make_unique<application::ApplicationPump>(std::move(*pump));
        std::vector<domain::LogFileIdentity> processed_files;
        processed_files.reserve(result->upload_history_store->records().size());
        for (const auto& record : result->upload_history_store->records()) {
            processed_files.push_back(record.file);
        }
        if (auto seeded = result->application_pump->seed_processed_logs(processed_files); !seeded) {
            return std::unexpected(runtime_error("Unable to seed persistent log history"));
        }
        return result;
    } catch (...) {
        return std::unexpected(runtime_error("Unexpected failure while composing the addon"));
    }
}

[[nodiscard]] std::expected<GeneralSettingsApplyReport, AddonRuntimeError>
apply_general_settings(RuntimeComponents& components, const config::GeneralSettings& previous,
                       const config::GeneralSettings& current,
                       std::filesystem::file_time_type minimum_last_write_time) {
    const bool source_changed = previous.log_directory != current.log_directory ||
                                previous.watch_subdirectories != current.watch_subdirectories ||
                                previous.max_candidates != current.max_candidates;
    const bool stability_changed =
        previous.stability_observations != current.stability_observations;

    if (source_changed) {
        auto reconfigured = components.candidate_source->reconfigure(
            std::filesystem::path{current.log_directory}, current.watch_subdirectories,
            current.max_candidates, minimum_last_write_time);
        if (!reconfigured) {
            return std::unexpected(runtime_error("Unable to apply log-directory settings"));
        }
        components.application_pump->reset_pending_candidates();
    }
    if (stability_changed) {
        auto updated = components.application_pump->update_required_matching_observations(
            current.stability_observations);
        if (!updated) {
            return std::unexpected(runtime_error("Unable to apply log-stability settings"));
        }
    }
    if (previous.parser_queue_capacity != current.parser_queue_capacity) {
        auto updated =
            components.metadata_worker->update_queue_capacity(current.parser_queue_capacity);
        if (!updated) {
            return std::unexpected(runtime_error("Unable to apply metadata-queue settings"));
        }
    }
    if (previous.recent_log_limit != current.recent_log_limit) {
        auto updated =
            components.upload_coordinator->update_history_limit(current.recent_log_limit);
        if (!updated) {
            return std::unexpected(runtime_error("Unable to apply recent-log history settings"));
        }
    }
    if (previous.parallel_uploads_per_provider != current.parallel_uploads_per_provider) {
        const auto parallelism = current.parallel_uploads_per_provider;
        if (!components.dps_report_worker->update_parallelism(parallelism) ||
            !components.wingman_worker->update_parallelism(parallelism) ||
            !components.donbot_worker->update_parallelism(parallelism) ||
            !components.twitch_worker->update_parallelism(parallelism)) {
            return std::unexpected(runtime_error("Unable to apply upload parallelism"));
        }
    }

    return GeneralSettingsApplyReport{
        .poll_now = source_changed || stability_changed ||
                    previous.poll_interval_ms != current.poll_interval_ms,
        .root_changed = source_changed,
    };
}

[[nodiscard]] std::string provider_state_text(domain::ProviderState state) {
    switch (state) {
    case domain::ProviderState::Disabled:
        return "Not selected";
    case domain::ProviderState::Waiting:
        return "Waiting";
    case domain::ProviderState::Active:
        return "Uploading";
    case domain::ProviderState::Succeeded:
        return "Succeeded";
    case domain::ProviderState::Skipped:
        return "Skipped";
    case domain::ProviderState::RetryScheduled:
        return "Retrying";
    case domain::ProviderState::Failed:
        return "Failed";
    case domain::ProviderState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

[[nodiscard]] std::string format_detected_at(domain::UploadJob::DetectedAt detected_at) {
    const auto whole_seconds = std::chrono::floor<std::chrono::seconds>(detected_at);
    const auto day = std::chrono::floor<std::chrono::days>(whole_seconds);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{whole_seconds - day};
    std::array<char, 32> text{};
    (void)std::snprintf(text.data(), text.size(), "%04d-%02u-%02u %02lld:%02lld:%02lld UTC",
                        static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                        static_cast<unsigned>(date.day()),
                        static_cast<long long>(time.hours().count()),
                        static_cast<long long>(time.minutes().count()),
                        static_cast<long long>(time.seconds().count()));
    return text.data();
}

[[nodiscard]] std::string format_health_left(std::uint16_t basis_points) {
    std::string formatted = std::to_string(basis_points / 100U);
    const auto fraction = basis_points % 100U;
    if (fraction != 0) {
        formatted.push_back('.');
        formatted += std::to_string(fraction / 10U);
        if ((fraction % 10U) != 0) {
            formatted += std::to_string(fraction % 10U);
        }
    }
    return formatted + "% left";
}

[[nodiscard]] RecentLogRow make_row(const application::UploadJobSnapshot& job) {
    RecentLogRow row;
    row.id = job.id.value;
    row.detected_at = format_detected_at(job.detected_at);
    row.filename = job.file.canonical_path.filename().string();
    row.report_available = job.dps_report_result.has_value();
    row.wingman_report_available = job.wingman_upload_receipt.has_value();
    row.directory_available = !job.file.canonical_path.parent_path().empty();
    const auto busy = [](domain::ProviderState state) {
        return state == domain::ProviderState::Waiting || state == domain::ProviderState::Active ||
               state == domain::ProviderState::RetryScheduled;
    };
    row.reupload_available =
        job.encounter_metadata.has_value() &&
        !busy(job.providers[domain::provider_index(domain::Provider::DpsReport)].state) &&
        !busy(job.providers[domain::provider_index(domain::Provider::Wingman)].state) &&
        !busy(job.providers[domain::provider_index(domain::Provider::DonBot)].state);
    row.rechat_available =
        job.dps_report_result.has_value() &&
        !busy(job.providers[domain::provider_index(domain::Provider::Twitch)].state);
    if (job.dps_report_result) {
        row.dps_report_url = job.dps_report_result->permalink;
        row.encounter_success = job.dps_report_result->success;
        row.encounter = job.dps_report_result->encounter_name;
        if (!job.dps_report_result->mode.empty()) {
            row.encounter += " (" + job.dps_report_result->mode + ")";
        }
        if (!job.dps_report_result->success && job.encounter_metadata &&
            job.encounter_metadata->remaining_health_basis_points) {
            row.encounter +=
                " (" + format_health_left(*job.encounter_metadata->remaining_health_basis_points) +
                ")";
        }
    } else if (job.encounter_metadata) {
        row.encounter = "Encounter " + std::to_string(job.encounter_metadata->boss_id);
    } else {
        row.encounter = "Reading metadata";
    }
    if (job.donbot_upload_receipt) {
        row.donbot_fight_log_id = job.donbot_upload_receipt->fight_log_id;
    }
    for (std::size_t index = 0; index < row.providers.size(); ++index) {
        row.providers[index] = ProviderCell{
            .state = job.providers[index].state,
            .status = provider_state_text(job.providers[index].state),
            .detail = job.providers[index].detail,
            .retry_available = job.providers[index].state == domain::ProviderState::Failed &&
                               job.encounter_metadata.has_value(),
        };
    }
    return row;
}

[[nodiscard]] RuntimeSnapshot publish_snapshot(const RuntimeComponents& components,
                                               std::string diagnostic, bool root_available,
                                               application::LogSelectionMode log_selection_mode,
                                               application::LogSelectionWindow log_selection_window,
                                               std::uint64_t revision) {
    const auto options_snapshot = components.options_controller->snapshot();
    RuntimeSnapshot snapshot{
        .options_snapshot = options_snapshot,
        .recent_log_actions = components.recent_log_actions->snapshot(),
        .options_model = ui::build_nexus_options_model(options_snapshot),
        .enabled_providers =
            config::enabled_provider_selection(options_snapshot.configuration.settings),
        .recent_logs = {},
        .dps_report_clipboard_text = {},
        .donbot_aggregate_url = {},
        .donbot_destination_status = "Off",
        .dps_report_url_count = 0,
        .donbot_fight_count = 0,
        .runtime_diagnostic = std::move(diagnostic),
        .log_root_available = root_available,
        .log_selection_mode = log_selection_mode,
        .log_selection_window = log_selection_window,
        .wine_secret_compatibility_mode = components.wine_secret_compatibility_mode,
        .twitch_application_configured = components.twitch_client->configured(),
        .revision = revision,
    };
    if (snapshot.enabled_providers[domain::provider_index(domain::Provider::DonBot)]) {
        snapshot.donbot_destination_status = "On";
        auto server_name = options_snapshot.configuration.settings.donbot.selected_guild_id;
        for (const auto& guild : options_snapshot.donbot.guilds) {
            if (guild.guild_id == server_name) {
                server_name = guild.guild_name;
                break;
            }
        }
        if (!server_name.empty()) {
            snapshot.donbot_destination_status += " (" + server_name + ")";
        }
    }
    auto jobs = components.upload_coordinator->snapshots();
    snapshot.recent_logs.reserve(jobs.size());
    for (auto iterator = jobs.rbegin(); iterator != jobs.rend(); ++iterator) {
        if (!application::log_matches_selection(iterator->file.last_write_time, log_selection_mode,
                                                log_selection_window)) {
            continue;
        }
        snapshot.recent_logs.push_back(make_row(*iterator));
    }
    std::string aggregate_ids;
    for (const auto& row : snapshot.recent_logs) {
        if (!row.dps_report_url.empty()) {
            if (!snapshot.dps_report_clipboard_text.empty()) {
                snapshot.dps_report_clipboard_text.push_back('\n');
            }
            snapshot.dps_report_clipboard_text += row.dps_report_url;
            ++snapshot.dps_report_url_count;
        }
        if (row.donbot_fight_log_id) {
            if (!aggregate_ids.empty()) {
                aggregate_ids.push_back(',');
            }
            aggregate_ids += std::to_string(*row.donbot_fight_log_id);
            ++snapshot.donbot_fight_count;
        }
    }
    if (!aggregate_ids.empty()) {
        snapshot.donbot_aggregate_url = std::string{providers::donbot_default_web_base} +
                                        "/logs/aggregate?ids=" + aggregate_ids;
    }
    return snapshot;
}

class ProductionRuntime final : public IAddonRuntime {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<IAddonRuntime>, AddonRuntimeError>
    create(std::unique_ptr<RuntimeComponents> components) {
        try {
            auto runtime =
                std::unique_ptr<ProductionRuntime>{new ProductionRuntime{std::move(components)}};
            runtime->owner_thread_ =
                std::jthread{[self = runtime.get()](std::stop_token token) { self->run(token); }};
            return runtime;
        } catch (...) {
            return std::unexpected(runtime_error("Unable to start the application owner thread"));
        }
    }

    ~ProductionRuntime() override {
        shutdown();
        std::fill(donbot_key_.begin(), donbot_key_.end(), '\0');
    }

    void render_main() override {
        auto snapshot = snapshot_copy();
        bool visible = window_visible_.load(std::memory_order_acquire);
        if (!visible) {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2{900.0F, 420.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(window_name, &visible)) {
            if (!snapshot.runtime_diagnostic.empty()) {
                ImGui::TextWrapped("%s", snapshot.runtime_diagnostic.c_str());
            }
            if (snapshot.recent_log_actions.last_error) {
                ImGui::TextWrapped("Action failed: %s",
                                   snapshot.recent_log_actions.last_error->message.c_str());
                if (ImGui::SmallButton("Dismiss action error")) {
                    submit_action(application::DismissRecentLogActionErrorCommand{});
                }
            }
            if (ImGui::RadioButton("Show New", snapshot.log_selection_mode ==
                                                   application::LogSelectionMode::New)) {
                select_log_mode(application::LogSelectionMode::New);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Includes logs completed after this addon load");
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Show Today", snapshot.log_selection_mode ==
                                                     application::LogSelectionMode::Today)) {
                select_log_mode(application::LogSelectionMode::Today);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Includes all logs completed since local midnight");
            }
            ImGui::Text("Log directory: %s",
                        snapshot.options_model.ordinary.general.log_directory.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(snapshot.log_root_available ? "(available)" : "(not found yet)");
            ImGui::Text(
                "Destinations: dps.report %s | GW2Wingman %s | DonBot %s | Twitch %s",
                snapshot.enabled_providers[domain::provider_index(domain::Provider::DpsReport)]
                    ? "On"
                    : "Off",
                snapshot.enabled_providers[domain::provider_index(domain::Provider::Wingman)]
                    ? "On"
                    : "Off",
                snapshot.donbot_destination_status.c_str(),
                snapshot.enabled_providers[domain::provider_index(domain::Provider::Twitch)]
                    ? "On"
                    : "Off");
            ImGui::Separator();

            if (!snapshot.dps_report_clipboard_text.empty() &&
                ImGui::Button("Copy dps.report URLs")) {
                ImGui::SetClipboardText(snapshot.dps_report_clipboard_text.c_str());
            }
            if (!snapshot.dps_report_clipboard_text.empty() &&
                !snapshot.donbot_aggregate_url.empty()) {
                ImGui::SameLine();
            }
            if (!snapshot.donbot_aggregate_url.empty() &&
                ImGui::Button("Copy DonBot aggregate URL")) {
                ImGui::SetClipboardText(snapshot.donbot_aggregate_url.c_str());
            }
            if (!snapshot.dps_report_clipboard_text.empty() ||
                !snapshot.donbot_aggregate_url.empty()) {
                ImGui::Separator();
            }

            if (snapshot.recent_logs.empty()) {
                ImGui::TextUnformatted("No logs detected yet.");
            } else if (ImGui::BeginTable("RecentLogs", 7,
                                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Detected");
                ImGui::TableSetupColumn("Encounter");
                ImGui::TableSetupColumn("dps.report");
                ImGui::TableSetupColumn("GW2Wingman");
                ImGui::TableSetupColumn("DonBot");
                ImGui::TableSetupColumn("Twitch");
                ImGui::TableSetupColumn("Actions");
                ImGui::TableHeadersRow();
                for (const auto& row : snapshot.recent_logs) {
                    ImGui::PushID(static_cast<int>(row.id & 0x7fffffffU));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.detected_at.c_str());
                    ImGui::TableSetColumnIndex(1);
                    if (row.encounter_success) {
                        const auto color = *row.encounter_success
                                               ? ImVec4{0.12F, 0.55F, 0.18F, 0.45F}
                                               : ImVec4{0.70F, 0.12F, 0.12F, 0.45F};
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                               ImGui::GetColorU32(color));
                    }
                    ImGui::TextUnformatted(row.encounter.c_str());
                    for (std::size_t index = 0; index < row.providers.size(); ++index) {
                        const auto& cell = row.providers[index];
                        const bool enabled_after_detection =
                            cell.state == domain::ProviderState::Disabled &&
                            snapshot.enabled_providers[index];
                        ImGui::TableSetColumnIndex(static_cast<int>(index + 2));
                        ImGui::TextUnformatted(enabled_after_detection ? "On - not sent"
                                                                       : cell.status.c_str());
                        if (enabled_after_detection && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                index == domain::provider_index(domain::Provider::Twitch)
                                    ? "Enabled now. This older log is not sent automatically; use "
                                      "Rechat to send it."
                                    : "Enabled now. This older log is not uploaded automatically; "
                                      "use Reupload to send it.");
                        } else if (!cell.detail.empty() && ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", cell.detail.c_str());
                        }
                        const bool view_available =
                            (index == domain::provider_index(domain::Provider::DpsReport) &&
                             row.report_available) ||
                            (index == domain::provider_index(domain::Provider::Wingman) &&
                             row.wingman_report_available) ||
                            (index == domain::provider_index(domain::Provider::DonBot) &&
                             row.donbot_fight_log_id.has_value());
                        if (view_available) {
                            if (ImGui::SmallButton(
                                    index == domain::provider_index(domain::Provider::DpsReport)
                                        ? "View report"
                                    : index == domain::provider_index(domain::Provider::Wingman)
                                        ? "View fight"
                                        : "View on DonBot")) {
                                if (index == domain::provider_index(domain::Provider::DpsReport)) {
                                    submit_action(application::OpenDpsReportCommand{
                                        .job_id = domain::UploadJobId{row.id}});
                                } else if (index ==
                                           domain::provider_index(domain::Provider::Wingman)) {
                                    submit_action(application::OpenWingmanReportCommand{
                                        .job_id = domain::UploadJobId{row.id}});
                                } else {
                                    submit_action(application::OpenDonBotReportCommand{
                                        .job_id = domain::UploadJobId{row.id}});
                                }
                            }
                        }
                        if (cell.retry_available) {
                            ImGui::PushID(static_cast<int>(index));
                            if (ImGui::SmallButton("Retry")) {
                                submit_action(application::RetryFailedProviderCommand{
                                    .job_id = domain::UploadJobId{row.id},
                                    .provider = static_cast<domain::Provider>(index),
                                });
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::TableSetColumnIndex(6);
                    if (row.directory_available && ImGui::SmallButton("Folder")) {
                        submit_action(application::OpenLogDirectoryCommand{
                            .job_id = domain::UploadJobId{row.id}});
                    }
                    if (row.directory_available && row.reupload_available) {
                        ImGui::SameLine();
                    }
                    if (row.reupload_available && ImGui::SmallButton("Reupload")) {
                        submit_action(
                            application::ReuploadLogCommand{.job_id = domain::UploadJobId{row.id}});
                    }
                    if ((row.directory_available || row.reupload_available) &&
                        row.rechat_available) {
                        ImGui::SameLine();
                    }
                    if (row.rechat_available && ImGui::SmallButton("Rechat")) {
                        submit_action(
                            application::RechatLogCommand{.job_id = domain::UploadJobId{row.id}});
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();

        if (!visible) {
            set_window_visible(false);
        }
    }

    void render_options() override {
        auto snapshot = snapshot_copy();
        initialize_draft(snapshot);
        ImGui::Separator();
        ImGui::TextUnformatted(window_name);

        bool window_visible = window_visible_.load(std::memory_order_acquire);
        if (ImGui::Checkbox("Show uploader window", &window_visible)) {
            set_window_visible(window_visible);
        }

        if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::InputText("Log directory", log_directory_.data(), log_directory_.size());
            ImGui::Checkbox("Watch subdirectories", &draft_.general.watch_subdirectories);
            ImGui::InputScalar("Poll interval (ms)", ImGuiDataType_U32,
                               &draft_.general.poll_interval_ms);
            ImGui::InputScalar("Stability observations", ImGuiDataType_U32,
                               &draft_.general.stability_observations);
            ImGui::InputScalar("Recent log limit", ImGuiDataType_U32,
                               &draft_.general.recent_log_limit);
            ImGui::InputScalar("Parser queue capacity", ImGuiDataType_U32,
                               &draft_.general.parser_queue_capacity);
            ImGui::InputScalar("Parallel uploads per provider", ImGuiDataType_U32,
                               &draft_.general.parallel_uploads_per_provider);
            ImGui::InputScalar("Maximum candidates", ImGuiDataType_U32,
                               &draft_.general.max_candidates);
            ImGui::TextWrapped("Saved general settings apply without reloading. Active parses and "
                               "uploads keep their already-captured inputs.");
        }

        if (ImGui::CollapsingHeader("Upload providers", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Upload to dps.report", &draft_.dps_report.enabled);
            ImGui::Checkbox("Upload to GW2Wingman", &draft_.wingman.enabled);
        }

        render_donbot(snapshot);
        render_twitch(snapshot);

        if (ImGui::Button("Save ordinary settings")) {
            submit_draft();
        }
        ImGui::SameLine();
        if (snapshot.options_model.command_pending) {
            ImGui::TextUnformatted("Applying...");
        }
        if (snapshot.options_model.last_error) {
            ImGui::TextWrapped("Error: %s", snapshot.options_model.last_error->c_str());
            if (ImGui::Button("Dismiss error")) {
                submit(application::DismissNexusOptionsErrorCommand{});
            }
        }
        ImGui::TextWrapped("Protected storage: %s",
                           snapshot.options_model.protected_storage_text.c_str());
        if (snapshot.wine_secret_compatibility_mode) {
            ImGui::TextWrapped("Wine compatibility mode: DonBot and Twitch "
                               "credentials are encrypted with Wine "
                               "DPAPI, which does not provide native Windows "
                               "user-scoped protection.");
        }
    }

    void toggle_window() override {
        bool visible = window_visible_.load(std::memory_order_acquire);
        while (!window_visible_.compare_exchange_weak(visible, !visible, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
        }
        submit(application::SetWindowVisibleCommand{.visible = !visible});
    }

    [[nodiscard]] QuickAccessStatus quick_access_status() const override {
        const auto snapshot = snapshot_copy();
        const auto& settings = snapshot.options_snapshot.configuration.settings;
        std::string donbot_guild;
        if (settings.donbot.enabled) {
            donbot_guild = settings.donbot.selected_guild_id;
            for (const auto& guild : snapshot.options_snapshot.donbot.guilds) {
                if (guild.guild_id == settings.donbot.selected_guild_id) {
                    donbot_guild = guild.guild_name;
                    break;
                }
            }
        }
        return make_quick_access_status(settings.dps_report.enabled, settings.wingman.enabled,
                                        settings.donbot.enabled, donbot_guild,
                                        settings.twitch.enabled);
    }

    void shutdown() noexcept override {
        bool expected = false;
        if (!shutting_down_.compare_exchange_strong(expected, true)) {
            return;
        }
        owner_thread_.request_stop();
        owner_condition_.notify_all();
        if (owner_thread_.joinable()) {
            owner_thread_.join();
        }
    }

  private:
    explicit ProductionRuntime(std::unique_ptr<RuntimeComponents> components)
        : components_{std::move(components)},
          published_{publish_snapshot(
              *components_, {}, false, application::LogSelectionMode::New,
              application::LogSelectionWindow{
                  .session_started_at = components_->session_started_at,
                  .local_day_started_at = local_day_started_at(components_->clock->system_now()),
              },
              1)},
          window_visible_{published_.options_model.ordinary.general.window_visible} {}

    template <typename Command> void submit(Command command) {
        (void)components_->options_controller->submit(
            application::NexusOptionsCommand{std::move(command)});
        owner_condition_.notify_all();
    }

    template <typename Command> void submit_action(Command command) {
        (void)components_->recent_log_actions->submit(
            application::RecentLogActionCommand{std::move(command)});
        owner_condition_.notify_all();
    }

    void initialize_draft(const RuntimeSnapshot& snapshot) {
        if (draft_initialized_) {
            return;
        }
        draft_ = snapshot.options_model.ordinary;
        copy_to_buffer(draft_.general.log_directory, log_directory_);
        copy_to_buffer(draft_.twitch_message_template, twitch_template_);
        copy_to_buffer(components_->twitch_client->client_id(), twitch_client_id_);
        copy_to_buffer(snapshot.options_snapshot.configuration.settings.donbot.api_base_url,
                       donbot_url_);
        draft_initialized_ = true;
    }

    template <std::size_t Size>
    static void copy_to_buffer(std::string_view text, std::array<char, Size>& buffer) {
        std::fill(buffer.begin(), buffer.end(), '\0');
        const auto length = std::min(text.size(), buffer.size() - 1);
        if (length > 0) {
            std::memcpy(buffer.data(), text.data(), length);
        }
    }

    void submit_draft() {
        draft_.general.log_directory = log_directory_.data();
        draft_.general.window_visible = window_visible_.load(std::memory_order_acquire);
        draft_.twitch_client_id = twitch_client_id_.data();
        draft_.twitch_message_template = twitch_template_.data();
        submit(application::SaveOrdinaryOptionsCommand{.options = draft_});
    }

    void set_window_visible(bool visible) {
        window_visible_.store(visible, std::memory_order_release);
        submit(application::SetWindowVisibleCommand{.visible = visible});
    }

    void select_log_mode(application::LogSelectionMode mode) {
        requested_log_selection_mode_.store(mode, std::memory_order_release);
        owner_condition_.notify_all();
    }

    void render_donbot(const RuntimeSnapshot& snapshot) {
        if (!ImGui::CollapsingHeader("DonBot", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        ImGui::TextWrapped("Status: %s", snapshot.options_model.donbot.status_text.c_str());
        if (!snapshot.options_model.donbot.diagnostic.empty()) {
            ImGui::TextWrapped("%s", snapshot.options_model.donbot.diagnostic.c_str());
        }
        const bool verified = snapshot.options_snapshot.donbot.state ==
                                  application::DonBotConfigurationState::Verified &&
                              snapshot.options_snapshot.donbot.account_name.has_value();
        if (!verified) {
            ImGui::InputText("DonBot API", donbot_url_.data(), donbot_url_.size());
            ImGui::InputText("GW2 API key", donbot_key_.data(), donbot_key_.size(),
                             ImGuiInputTextFlags_Password);
            if (snapshot.options_model.donbot.verify_available && ImGui::Button("Verify DonBot")) {
                auto key = support::SecretValue::from_text(donbot_key_.data());
                submit(application::VerifyDonBotCommand{
                    .api_base_url = donbot_url_.data(),
                    .api_key = std::move(key),
                });
                std::fill(donbot_key_.begin(), donbot_key_.end(), '\0');
            }
            return;
        }

        const auto& selected_id = snapshot.options_snapshot.donbot.selected_guild_id;
        std::string selected_name = "Select a server";
        for (const auto& guild : snapshot.options_snapshot.donbot.guilds) {
            if (guild.guild_id == selected_id) {
                selected_name = guild.guild_name;
                break;
            }
        }
        if (snapshot.options_model.donbot.guild_selection_available) {
            ImGui::SetNextItemWidth(280.0F);
            if (ImGui::BeginCombo("Server##DonBotServer", selected_name.c_str())) {
                for (const auto& guild : snapshot.options_snapshot.donbot.guilds) {
                    const bool selected = selected_id == guild.guild_id;
                    if (ImGui::Selectable(guild.guild_name.c_str(), selected)) {
                        submit(application::SelectDonBotGuildCommand{.guild_id = guild.guild_id});
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            bool enabled = snapshot.options_snapshot.configuration.settings.donbot.enabled;
            if (ImGui::Checkbox("Enable uploads##DonBot", &enabled)) {
                submit(application::SetDonBotEnabledCommand{.enabled = enabled});
            }
        }
        ImGui::TextWrapped("Provider changes apply to newly detected logs. Existing delivery "
                           "history is never replayed automatically.");
        if (snapshot.options_model.donbot.disconnect_available &&
            ImGui::Button("Deverify DonBot")) {
            submit(application::DisconnectDonBotCommand{});
        }
    }

    void render_twitch(const RuntimeSnapshot& snapshot) {
        if (!ImGui::CollapsingHeader("Twitch", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        if (!snapshot.twitch_application_configured) {
            ImGui::TextWrapped("Enter the public Client ID from your Twitch developer application, "
                               "save settings, then connect the broadcaster account. A client "
                               "secret is not used or stored.");
        }
        const auto twitch_state = snapshot.options_snapshot.twitch.state;
        const bool client_id_editable =
            twitch_state == application::TwitchConnectionState::Disconnected ||
            twitch_state == application::TwitchConnectionState::Error;
        ImGui::InputText(
            "Application client ID", twitch_client_id_.data(), twitch_client_id_.size(),
            client_id_editable ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Open dev.twitch.tv/console/apps and register an application.\n"
                "Use a unique name, add http://localhost:3000 as the redirect URL if required, "
                "choose a suitable category, and select Public as the client type.\n"
                "After creation, open Manage and copy its Client ID into this field.\n"
                "Do not create or paste a Client Secret; MannyUploader does not use one.");
        }
        ImGui::TextWrapped("Status: %s", snapshot.options_model.twitch.status_text.c_str());
        if (!snapshot.options_model.twitch.diagnostic.empty()) {
            ImGui::TextWrapped("%s", snapshot.options_model.twitch.diagnostic.c_str());
        }
        if (snapshot.options_model.twitch.user_code) {
            ImGui::Text("Code: %s", snapshot.options_model.twitch.user_code->c_str());
        }
        if (snapshot.options_model.twitch.verification_uri) {
            ImGui::TextWrapped("Open: %s", snapshot.options_model.twitch.verification_uri->c_str());
        }
        if (snapshot.twitch_application_configured &&
            snapshot.options_model.twitch.connect_available && ImGui::Button("Connect Twitch")) {
            submit(application::ConnectTwitchCommand{});
        }
        if (snapshot.options_model.twitch.enable_toggle_available) {
            bool enabled = snapshot.options_snapshot.configuration.settings.twitch.enabled;
            if (ImGui::Checkbox("Enable Twitch chat upload", &enabled)) {
                submit(application::SetTwitchEnabledCommand{.enabled = enabled});
            }
        }
        if (snapshot.options_model.twitch.disconnect_available &&
            ImGui::Button("Disconnect Twitch")) {
            submit(application::DisconnectTwitchCommand{});
        }
        if (snapshot.options_model.twitch.test_message_available &&
            ImGui::Button("Send test message")) {
            submit(application::SendTwitchTestMessageCommand{});
        }
        ImGui::TextWrapped("Test message: %s",
                           snapshot.options_model.twitch.test_message_status_text.c_str());
        ImGui::InputTextMultiline("Message template", twitch_template_.data(),
                                  twitch_template_.size(), ImVec2{-1.0F, 70.0F});
        ImGui::Checkbox("Post successful encounters", &draft_.twitch_post_success);
        ImGui::Checkbox("Post failed encounters", &draft_.twitch_post_failure);
    }

    [[nodiscard]] RuntimeSnapshot snapshot_copy() const {
        const std::scoped_lock lock{published_mutex_};
        return published_;
    }

    void set_published(RuntimeSnapshot snapshot) {
        const std::scoped_lock lock{published_mutex_};
        published_ = std::move(snapshot);
    }

    void run(const std::stop_token& stop_token) noexcept {
        std::string diagnostic = components_->startup_diagnostic;
        bool root_available{};
        std::uint64_t revision{2};
        auto next_poll = components_->clock->steady_now();
        auto applied_log_selection_mode = application::LogSelectionMode::New;
        auto log_selection_window = application::LogSelectionWindow{
            .session_started_at = components_->session_started_at,
            .local_day_started_at = local_day_started_at(components_->clock->system_now()),
        };
        auto applied_log_cutoff =
            application::log_selection_cutoff(applied_log_selection_mode, log_selection_window);

        try {
            const auto initial = components_->configuration->snapshot();
            auto applied_general_settings = initial.settings.general;
            auto applied_settings_revision = initial.revision;
            auto last_persisted_records = components_->upload_history_store->records();
            if (components_->persistent_secrets_available &&
                !initial.settings.donbot.selected_guild_id.empty()) {
                (void)components_->donbot_controller->begin_saved_verification();
            }
            if (components_->persistent_secrets_available &&
                components_->twitch_client->configured()) {
                (void)components_->twitch_controller->begin_saved_connection();
            }

            while (!stop_token.stop_requested()) {
                auto options_tick = components_->options_controller->tick();
                if (!options_tick) {
                    diagnostic = options_tick.error().message;
                }
                auto action_tick = components_->recent_log_actions->tick();
                if (!action_tick && action_tick.error().code !=
                                        application::RecentLogActionErrorCode::ShuttingDown) {
                    diagnostic = action_tick.error().message;
                }

                const auto configuration = components_->configuration->snapshot();
                if (configuration.revision != applied_settings_revision) {
                    applied_settings_revision = configuration.revision;
                    if (configuration.settings.general != applied_general_settings) {
                        auto applied = apply_general_settings(
                            *components_, applied_general_settings, configuration.settings.general,
                            applied_log_cutoff);
                        if (!applied) {
                            diagnostic = applied.error().message;
                        } else {
                            applied_general_settings = configuration.settings.general;
                            if (applied->root_changed) {
                                root_available = false;
                            }
                            if (applied->poll_now) {
                                next_poll = components_->clock->steady_now();
                            }
                        }
                    }
                    (void)components_->donbot_worker->update_config(providers::DonBotProviderConfig{
                        .api_base_url = configuration.settings.donbot.api_base_url,
                        .guild_id = configuration.settings.donbot.selected_guild_id,
                    });
                    if (auto updated = components_->twitch_client->update_client_id(
                            effective_twitch_client_id(configuration.settings));
                        !updated) {
                        diagnostic = updated.error().detail;
                    }
                    (void)components_->twitch_worker->update_config(providers::TwitchProviderConfig{
                        .message_template = configuration.settings.twitch.message_template,
                        .post_success = configuration.settings.twitch.post_success,
                        .post_failure = configuration.settings.twitch.post_failure,
                    });
                }

                auto desired_window = application::LogSelectionWindow{
                    .session_started_at = components_->session_started_at,
                    .local_day_started_at = local_day_started_at(components_->clock->system_now()),
                };
                const auto desired_mode =
                    requested_log_selection_mode_.load(std::memory_order_acquire);
                const auto desired_cutoff =
                    application::log_selection_cutoff(desired_mode, desired_window);
                if (desired_mode != applied_log_selection_mode ||
                    desired_cutoff != applied_log_cutoff) {
                    auto reconfigured = components_->candidate_source->reconfigure(
                        std::filesystem::path{configuration.settings.general.log_directory},
                        configuration.settings.general.watch_subdirectories,
                        configuration.settings.general.max_candidates, desired_cutoff);
                    if (!reconfigured) {
                        diagnostic = "Unable to apply the log selection";
                        requested_log_selection_mode_.store(applied_log_selection_mode,
                                                            std::memory_order_release);
                    } else {
                        components_->application_pump->reset_pending_candidates();
                        applied_log_selection_mode = desired_mode;
                        applied_log_cutoff = desired_cutoff;
                        root_available = false;
                        next_poll = components_->clock->steady_now();
                    }
                }
                log_selection_window = desired_window;

                const auto now = components_->clock->steady_now();
                if (now >= next_poll) {
                    auto report = components_->application_pump->tick(
                        config::enabled_provider_selection(configuration.settings), stop_token);
                    if (report) {
                        root_available = report->root_available;
                        if (!report->source_issues.empty()) {
                            diagnostic = report->source_issues.front().message;
                        } else if (!root_available) {
                            diagnostic = "Waiting for the configured log directory";
                        } else {
                            diagnostic.clear();
                        }
                    } else if (report.error().code !=
                               application::ApplicationPumpErrorCode::ShuttingDown) {
                        diagnostic = report.error().message;
                    }
                    next_poll = now + std::chrono::milliseconds{
                                          configuration.settings.general.poll_interval_ms};
                }

                auto history_records = components_->upload_coordinator->history_records();
                if (history_records != last_persisted_records) {
                    if (auto saved =
                            components_->upload_history_store->merge_and_save(history_records);
                        !saved) {
                        diagnostic = "Unable to persist upload history: " + saved.error().message;
                    } else {
                        last_persisted_records = std::move(history_records);
                    }
                }

                set_published(publish_snapshot(*components_, diagnostic, root_available,
                                               applied_log_selection_mode, log_selection_window,
                                               revision++));
                std::unique_lock lock{owner_mutex_};
                owner_condition_.wait_for(lock, 50ms);
            }
        } catch (...) {
            diagnostic = "The application owner stopped after an unexpected failure";
            try {
                set_published(publish_snapshot(*components_, diagnostic, root_available,
                                               applied_log_selection_mode, log_selection_window,
                                               revision++));
            } catch (...) {
            }
        }

        components_->recent_log_actions->shutdown();
        components_->options_controller->shutdown();
        components_->application_pump->cancel_all();
        (void)components_->upload_history_store->merge_and_save(
            components_->upload_coordinator->history_records());
        components_->donbot_controller->shutdown();
        components_->twitch_controller->shutdown();
        components_->twitch_session_owner->shutdown();
        components_->configuration->shutdown();
    }

    std::unique_ptr<RuntimeComponents> components_;
    mutable std::mutex published_mutex_;
    RuntimeSnapshot published_;
    std::mutex owner_mutex_;
    std::condition_variable_any owner_condition_;
    std::jthread owner_thread_;
    std::atomic_bool shutting_down_{};
    std::atomic_bool window_visible_;
    std::atomic<application::LogSelectionMode> requested_log_selection_mode_{
        application::LogSelectionMode::New};

    bool draft_initialized_{};
    application::NexusOrdinaryOptions draft_;
    std::array<char, 4097> log_directory_{};
    std::array<char, 2049> twitch_template_{};
    std::array<char, 129> twitch_client_id_{};
    std::array<char, 2049> donbot_url_{};
    std::array<char, 513> donbot_key_{};
};

} // namespace

std::expected<std::unique_ptr<IAddonRuntime>, AddonRuntimeError>
create_production_runtime(const AddonPaths& paths) {
    auto components = create_components(paths);
    if (!components) {
        return std::unexpected(std::move(components.error()));
    }
    return ProductionRuntime::create(std::move(*components));
}

} // namespace manny_uploader::addon
