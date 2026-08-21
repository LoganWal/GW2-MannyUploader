#include "manny_uploader/application/configuration_service.hpp"

#include "support/test_suite.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using application::ConfigurationErrorCode;
using application::ConfigurationService;
using application::PersistentSecretStorageState;
using ports::SecretId;

[[nodiscard]] config::Settings test_settings() {
    return config::make_default_settings("C:/Users/Streamer/Documents/Guild Wars 2/logs");
}

[[nodiscard]] ports::SettingsLoadResult
load_result(ports::SettingsLoadSource source = ports::SettingsLoadSource::Defaults,
            std::optional<std::string> diagnostic = std::nullopt) {
    return ports::SettingsLoadResult{
        .settings = test_settings(),
        .source = source,
        .recovery_diagnostic = std::move(diagnostic),
    };
}

[[nodiscard]] ports::SettingsStoreError settings_error(ports::SettingsStoreErrorCode code,
                                                       std::string message) {
    return ports::SettingsStoreError{
        .code = code,
        .message = std::move(message),
        .path = "C:/addon/settings.json",
        .validation_errors = {},
    };
}

[[nodiscard]] ports::SecretStoreError
secret_error(ports::SecretStoreErrorCode code, std::string message,
             std::optional<std::uint32_t> system_error = std::nullopt) {
    return ports::SecretStoreError{
        .code = code,
        .id = std::nullopt,
        .message = std::move(message),
        .system_error = system_error,
    };
}

class RecordingSettingsStore final : public ports::ISettingsStore {
  public:
    explicit RecordingSettingsStore(ports::SettingsLoadResult result = load_result())
        : result_{std::move(result)} {}

    [[nodiscard]] std::expected<ports::SettingsLoadResult, ports::SettingsStoreError>
    load() const override {
        ++load_count;
        if (load_failure) {
            return std::unexpected(*load_failure);
        }
        return result_;
    }

    [[nodiscard]] std::expected<void, ports::SettingsStoreError>
    save(const config::Settings& settings) const override {
        ++save_count;
        if (save_failure) {
            return std::unexpected(*save_failure);
        }
        saved_settings.push_back(settings);
        return {};
    }

    ports::SettingsLoadResult result_;
    mutable std::optional<ports::SettingsStoreError> load_failure;
    mutable std::optional<ports::SettingsStoreError> save_failure;
    mutable std::vector<config::Settings> saved_settings;
    mutable std::size_t load_count{};
    mutable std::size_t save_count{};
};

class RecordingSecretStore final : public ports::ISecretStore {
  public:
    [[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
    load(SecretId id) const override {
        load_ids.push_back(id);
        if (load_failure) {
            return std::unexpected(*load_failure);
        }
        return support::SecretValue{load_bytes};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError>
    store(SecretId id, const support::SecretValue& value) override {
        store_ids.push_back(id);
        if (store_failure) {
            return std::unexpected(*store_failure);
        }
        stored_bytes.assign(value.bytes().begin(), value.bytes().end());
        return {};
    }

    [[nodiscard]] std::expected<void, ports::SecretStoreError> erase(SecretId id) override {
        erase_ids.push_back(id);
        if (erase_failure) {
            return std::unexpected(*erase_failure);
        }
        return {};
    }

    std::vector<std::byte> load_bytes;
    std::optional<ports::SecretStoreError> load_failure;
    std::optional<ports::SecretStoreError> store_failure;
    std::optional<ports::SecretStoreError> erase_failure;
    mutable std::vector<SecretId> load_ids;
    std::vector<SecretId> store_ids;
    std::vector<SecretId> erase_ids;
    std::vector<std::byte> stored_bytes;
};

[[nodiscard]] bool snapshot_contains(const application::ConfigurationSnapshot& snapshot,
                                     std::string_view marker) {
    const auto contains = [marker](std::string_view text) {
        return text.find(marker) != text.npos;
    };
    return contains(snapshot.settings.general.log_directory) ||
           contains(snapshot.settings.donbot.api_base_url) ||
           contains(snapshot.settings.donbot.selected_guild_id) ||
           contains(snapshot.settings.twitch.message_template) ||
           contains(snapshot.settings_recovery_diagnostic.value_or("")) ||
           contains(snapshot.persistent_secret_storage.diagnostic);
}

void creation_tests(TestSuite& suite) {
    auto invalid_settings = ConfigurationService::create(nullptr, nullptr);
    MANNY_CHECK(suite, !invalid_settings.has_value());
    MANNY_CHECK(suite,
                invalid_settings.error().code == ConfigurationErrorCode::InvalidConfiguration);

    auto missing_secret =
        ConfigurationService::create(std::make_unique<RecordingSettingsStore>(), nullptr);
    MANNY_CHECK(suite, !missing_secret.has_value());
    MANNY_CHECK(suite, missing_secret.error().code == ConfigurationErrorCode::InvalidConfiguration);

    auto settings_store = std::make_unique<RecordingSettingsStore>(
        load_result(ports::SettingsLoadSource::Backup, "Recovered the last-known-good settings"));
    auto* settings_observer = settings_store.get();
    auto secret_store = std::make_unique<RecordingSecretStore>();
    auto service = ConfigurationService::create(std::move(settings_store), std::move(secret_store));
    MANNY_CHECK(suite, service.has_value());
    if (!service) {
        return;
    }

    const auto snapshot = service->snapshot();
    MANNY_CHECK(suite, settings_observer->load_count == 1);
    MANNY_CHECK(suite, snapshot.settings == test_settings());
    MANNY_CHECK(suite, snapshot.settings_load_source == ports::SettingsLoadSource::Backup);
    MANNY_CHECK(suite, snapshot.settings_recovery_diagnostic.has_value());
    MANNY_CHECK(suite,
                snapshot.settings_recovery_diagnostic == "Recovered the last-known-good settings");
    MANNY_CHECK(suite, snapshot.persistent_secret_storage.state ==
                           PersistentSecretStorageState::Available);
    MANNY_CHECK(suite, !snapshot.persistent_secret_storage.error_code.has_value());
    MANNY_CHECK(suite, snapshot.persistent_secret_storage.diagnostic.empty());
    MANNY_CHECK(suite, snapshot.revision == 1);
    MANNY_CHECK(suite, !snapshot.shutting_down);

    auto failing_store = std::make_unique<RecordingSettingsStore>();
    failing_store->load_failure =
        settings_error(ports::SettingsStoreErrorCode::NoValidSettings, "No valid settings exist");
    failing_store->load_failure->validation_errors.push_back(config::SettingsValidationError{
        .code = config::SettingsValidationErrorCode::OutOfRange,
        .field = "general.recent_log_limit",
        .message = "Recent log limit is invalid",
    });
    auto failed = ConfigurationService::create(std::move(failing_store),
                                               std::make_unique<RecordingSecretStore>());
    MANNY_CHECK(suite, !failed.has_value());
    MANNY_CHECK(suite, failed.error().code == ConfigurationErrorCode::SettingsLoadFailed);
    MANNY_CHECK(suite,
                failed.error().settings_error == ports::SettingsStoreErrorCode::NoValidSettings);
    MANNY_CHECK(suite, failed.error().settings_path == "C:/addon/settings.json");
    MANNY_CHECK(suite, failed.error().settings_validation_errors.size() == 1);
    MANNY_CHECK(suite, !failed.error().secret_error.has_value());
}

void unavailable_storage_tests(TestSuite& suite) {
    const auto unsupported = secret_error(ports::SecretStoreErrorCode::UnsupportedEnvironment,
                                          "Secure persistence is unavailable under Wine", 50);
    auto service = ConfigurationService::create_without_secret_storage(
        std::make_unique<RecordingSettingsStore>(), unsupported);
    MANNY_CHECK(suite, service.has_value());
    if (!service) {
        return;
    }

    const auto snapshot = service->snapshot();
    MANNY_CHECK(suite, snapshot.persistent_secret_storage.state ==
                           PersistentSecretStorageState::UnsupportedEnvironment);
    MANNY_CHECK(suite, snapshot.persistent_secret_storage.error_code ==
                           ports::SecretStoreErrorCode::UnsupportedEnvironment);
    MANNY_CHECK(suite, snapshot.persistent_secret_storage.diagnostic ==
                           "Secure persistence is unavailable under Wine");
    MANNY_CHECK(suite, snapshot.persistent_secret_storage.system_error == 50);

    const auto marker = support::SecretValue::from_text("must-not-enter-unavailable-error");
    const auto loaded = service->load_secret(SecretId::DpsReportUserToken);
    const auto stored = service->store_secret(SecretId::DonBotGw2ApiKey, marker);
    const auto erased = service->erase_secret(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, !stored.has_value());
    MANNY_CHECK(suite, !erased.has_value());
    MANNY_CHECK(suite, loaded.error().code == ConfigurationErrorCode::SecretStorageUnavailable);
    MANNY_CHECK(suite, stored.error().code == ConfigurationErrorCode::SecretStorageUnavailable);
    MANNY_CHECK(suite, erased.error().code == ConfigurationErrorCode::SecretStorageUnavailable);
    MANNY_CHECK(suite, loaded.error().secret_id == SecretId::DpsReportUserToken);
    MANNY_CHECK(suite, stored.error().secret_id == SecretId::DonBotGw2ApiKey);
    MANNY_CHECK(suite, erased.error().secret_id == SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, loaded.error().message.find("must-not-enter-unavailable-error") ==
                           std::string::npos);

    auto failed = ConfigurationService::create_without_secret_storage(
        std::make_unique<RecordingSettingsStore>(),
        secret_error(ports::SecretStoreErrorCode::DirectoryCreateFailed,
                     "Could not initialize protected storage"));
    MANNY_CHECK(suite, failed.has_value());
    if (failed) {
        MANNY_CHECK(suite, failed->snapshot().persistent_secret_storage.state ==
                               PersistentSecretStorageState::InitializationFailed);
    }
}

void settings_write_through_tests(TestSuite& suite) {
    auto settings_store = std::make_unique<RecordingSettingsStore>(
        load_result(ports::SettingsLoadSource::Backup, "Recovered backup"));
    auto* observer = settings_store.get();
    auto service = ConfigurationService::create(std::move(settings_store),
                                                std::make_unique<RecordingSecretStore>());
    MANNY_CHECK(suite, service.has_value());
    if (!service) {
        return;
    }

    auto updated = test_settings();
    updated.general.recent_log_limit = 99;
    MANNY_CHECK(suite, service->save_settings(updated).has_value());
    MANNY_CHECK(suite, observer->save_count == 1);
    MANNY_CHECK(suite, observer->saved_settings.size() == 1);
    MANNY_CHECK(suite, observer->saved_settings.front() == updated);
    auto snapshot = service->snapshot();
    MANNY_CHECK(suite, snapshot.settings == updated);
    MANNY_CHECK(suite, snapshot.settings_load_source == ports::SettingsLoadSource::Primary);
    MANNY_CHECK(suite, !snapshot.settings_recovery_diagnostic.has_value());
    MANNY_CHECK(suite, snapshot.revision == 2);

    observer->save_failure =
        settings_error(ports::SettingsStoreErrorCode::ValidationFailed, "Settings are invalid");
    observer->save_failure->validation_errors.push_back(config::SettingsValidationError{
        .code = config::SettingsValidationErrorCode::OutOfRange,
        .field = "general.recent_log_limit",
        .message = "Recent log limit is invalid",
    });
    auto rejected = updated;
    rejected.general.recent_log_limit = 100;
    const auto failed = service->save_settings(rejected);
    MANNY_CHECK(suite, !failed.has_value());
    MANNY_CHECK(suite, failed.error().code == ConfigurationErrorCode::SettingsSaveFailed);
    MANNY_CHECK(suite,
                failed.error().settings_error == ports::SettingsStoreErrorCode::ValidationFailed);
    MANNY_CHECK(suite, failed.error().settings_validation_errors.size() == 1);
    MANNY_CHECK(suite, observer->save_count == 2);
    MANNY_CHECK(suite, observer->saved_settings.size() == 1);
    snapshot = service->snapshot();
    MANNY_CHECK(suite, snapshot.settings == updated);
    MANNY_CHECK(suite, snapshot.revision == 2);
}

void secret_routing_tests(TestSuite& suite) {
    constexpr std::string_view marker = "configuration-service-secret-marker";
    auto secret_store = std::make_unique<RecordingSecretStore>();
    auto* observer = secret_store.get();
    const auto source = support::SecretValue::from_text(marker);
    observer->load_bytes.assign(source.bytes().begin(), source.bytes().end());
    auto service = ConfigurationService::create(std::make_unique<RecordingSettingsStore>(),
                                                std::move(secret_store));
    MANNY_CHECK(suite, service.has_value());
    if (!service) {
        return;
    }

    auto loaded = service->load_secret(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, loaded.has_value());
    if (loaded) {
        MANNY_CHECK(suite, *loaded == source);
    }
    MANNY_CHECK(suite, observer->load_ids.size() == 1);
    MANNY_CHECK(suite, observer->load_ids.front() == SecretId::TwitchOAuthSession);

    MANNY_CHECK(suite, service->store_secret(SecretId::DonBotGw2ApiKey, source).has_value());
    MANNY_CHECK(suite, observer->store_ids.size() == 1);
    MANNY_CHECK(suite, observer->store_ids.front() == SecretId::DonBotGw2ApiKey);
    MANNY_CHECK(suite, std::ranges::equal(observer->stored_bytes, source.bytes()));

    MANNY_CHECK(suite, service->erase_secret(SecretId::DpsReportUserToken).has_value());
    MANNY_CHECK(suite, observer->erase_ids.size() == 1);
    MANNY_CHECK(suite, observer->erase_ids.front() == SecretId::DpsReportUserToken);
    MANNY_CHECK(suite, !snapshot_contains(service->snapshot(), marker));
    MANNY_CHECK(suite, service->snapshot().revision == 1);

    observer->load_failure =
        secret_error(ports::SecretStoreErrorCode::NotFound, "Credential does not exist");
    observer->store_failure = secret_error(ports::SecretStoreErrorCode::ProtectionFailed,
                                           "Credential protection failed", 5);
    observer->erase_failure =
        secret_error(ports::SecretStoreErrorCode::DeleteFailed, "Credential deletion failed");

    const auto load_failed = service->load_secret(SecretId::DpsReportUserToken);
    const auto store_failed = service->store_secret(SecretId::TwitchOAuthSession, source);
    const auto erase_failed = service->erase_secret(SecretId::DonBotGw2ApiKey);
    MANNY_CHECK(suite, !load_failed.has_value());
    MANNY_CHECK(suite, !store_failed.has_value());
    MANNY_CHECK(suite, !erase_failed.has_value());
    MANNY_CHECK(suite, load_failed.error().code == ConfigurationErrorCode::SecretLoadFailed);
    MANNY_CHECK(suite, store_failed.error().code == ConfigurationErrorCode::SecretStoreFailed);
    MANNY_CHECK(suite, erase_failed.error().code == ConfigurationErrorCode::SecretEraseFailed);
    MANNY_CHECK(suite, load_failed.error().secret_error == ports::SecretStoreErrorCode::NotFound);
    MANNY_CHECK(suite,
                store_failed.error().secret_error == ports::SecretStoreErrorCode::ProtectionFailed);
    MANNY_CHECK(suite,
                erase_failed.error().secret_error == ports::SecretStoreErrorCode::DeleteFailed);
    MANNY_CHECK(suite, store_failed.error().system_error == 5);
    MANNY_CHECK(suite, store_failed.error().message.find(marker) == std::string::npos);
}

void shutdown_tests(TestSuite& suite) {
    auto settings_store = std::make_unique<RecordingSettingsStore>();
    auto* settings_observer = settings_store.get();
    auto secret_store = std::make_unique<RecordingSecretStore>();
    auto* secret_observer = secret_store.get();
    auto service = ConfigurationService::create(std::move(settings_store), std::move(secret_store));
    MANNY_CHECK(suite, service.has_value());
    if (!service) {
        return;
    }

    service->shutdown();
    MANNY_CHECK(suite, service->is_shutting_down());
    MANNY_CHECK(suite, service->snapshot().shutting_down);
    MANNY_CHECK(suite, service->snapshot().revision == 2);
    service->shutdown();
    MANNY_CHECK(suite, service->snapshot().revision == 2);

    const auto secret = support::SecretValue::from_text("shutdown-secret-marker");
    const auto saved = service->save_settings(test_settings());
    const auto loaded = service->load_secret(SecretId::DpsReportUserToken);
    const auto stored = service->store_secret(SecretId::DonBotGw2ApiKey, secret);
    const auto erased = service->erase_secret(SecretId::TwitchOAuthSession);
    MANNY_CHECK(suite, !saved.has_value());
    MANNY_CHECK(suite, !loaded.has_value());
    MANNY_CHECK(suite, !stored.has_value());
    MANNY_CHECK(suite, !erased.has_value());
    MANNY_CHECK(suite, saved.error().code == ConfigurationErrorCode::ShuttingDown);
    MANNY_CHECK(suite, loaded.error().code == ConfigurationErrorCode::ShuttingDown);
    MANNY_CHECK(suite, stored.error().code == ConfigurationErrorCode::ShuttingDown);
    MANNY_CHECK(suite, erased.error().code == ConfigurationErrorCode::ShuttingDown);
    MANNY_CHECK(suite, settings_observer->save_count == 0);
    MANNY_CHECK(suite, secret_observer->load_ids.empty());
    MANNY_CHECK(suite, secret_observer->store_ids.empty());
    MANNY_CHECK(suite, secret_observer->erase_ids.empty());
}

} // namespace

void run_configuration_service_tests(TestSuite& suite) {
    creation_tests(suite);
    unavailable_storage_tests(suite);
    settings_write_through_tests(suite);
    secret_routing_tests(suite);
    shutdown_tests(suite);
}

} // namespace manny_uploader::test
