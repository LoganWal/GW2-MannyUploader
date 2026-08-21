#include "manny_uploader/application/configuration_service.hpp"

#include <limits>
#include <utility>

namespace manny_uploader::application {
namespace {

[[nodiscard]] ConfigurationError make_error(ConfigurationErrorCode code, std::string message) {
    return ConfigurationError{
        .code = code,
        .message = std::move(message),
        .settings_error = std::nullopt,
        .secret_error = std::nullopt,
        .secret_id = std::nullopt,
        .system_error = std::nullopt,
        .settings_path = std::nullopt,
        .settings_validation_errors = {},
    };
}

[[nodiscard]] ConfigurationError map_settings_error(ConfigurationErrorCode code,
                                                    const ports::SettingsStoreError& error) {
    auto mapped = make_error(code, error.message);
    mapped.settings_error = error.code;
    mapped.settings_path = error.path;
    mapped.settings_validation_errors = error.validation_errors;
    return mapped;
}

[[nodiscard]] ConfigurationError map_secret_error(ConfigurationErrorCode code,
                                                  ports::SecretId requested_id,
                                                  const ports::SecretStoreError& error) {
    auto mapped = make_error(code, error.message);
    mapped.secret_error = error.code;
    mapped.secret_id = error.id.value_or(requested_id);
    mapped.system_error = error.system_error;
    return mapped;
}

[[nodiscard]] ConfigurationError shutting_down_error(std::optional<ports::SecretId> id) {
    auto error =
        make_error(ConfigurationErrorCode::ShuttingDown, "Configuration service is shutting down");
    error.secret_id = id;
    return error;
}

} // namespace

ConfigurationService::ConfigurationService(std::unique_ptr<ports::ISettingsStore> settings_store,
                                           std::unique_ptr<ports::ISecretStore> secret_store,
                                           ConfigurationSnapshot snapshot)
    : settings_store_{std::move(settings_store)}, secret_store_{std::move(secret_store)},
      snapshot_{std::move(snapshot)} {}

std::expected<ConfigurationService, ConfigurationError>
ConfigurationService::create(std::unique_ptr<ports::ISettingsStore> settings_store,
                             std::unique_ptr<ports::ISecretStore> secret_store) {
    return create_impl(std::move(settings_store), std::move(secret_store),
                       PersistentSecretStorageSnapshot{
                           .state = PersistentSecretStorageState::Available,
                           .error_code = std::nullopt,
                           .diagnostic = {},
                           .system_error = std::nullopt,
                       });
}

std::expected<ConfigurationService, ConfigurationError>
ConfigurationService::create_without_secret_storage(
    std::unique_ptr<ports::ISettingsStore> settings_store,
    ports::SecretStoreError unavailable_reason) {
    const auto state =
        unavailable_reason.code == ports::SecretStoreErrorCode::UnsupportedEnvironment
            ? PersistentSecretStorageState::UnsupportedEnvironment
            : PersistentSecretStorageState::InitializationFailed;
    return create_impl(std::move(settings_store), nullptr,
                       PersistentSecretStorageSnapshot{
                           .state = state,
                           .error_code = unavailable_reason.code,
                           .diagnostic = std::move(unavailable_reason.message),
                           .system_error = unavailable_reason.system_error,
                       });
}

std::expected<ConfigurationService, ConfigurationError>
ConfigurationService::create_impl(std::unique_ptr<ports::ISettingsStore> settings_store,
                                  std::unique_ptr<ports::ISecretStore> secret_store,
                                  PersistentSecretStorageSnapshot secret_storage) {
    if (!settings_store) {
        return std::unexpected(make_error(ConfigurationErrorCode::InvalidConfiguration,
                                          "A settings store is required"));
    }
    const auto expects_secret_store =
        secret_storage.state == PersistentSecretStorageState::Available;
    if (expects_secret_store != static_cast<bool>(secret_store)) {
        return std::unexpected(
            make_error(ConfigurationErrorCode::InvalidConfiguration,
                       "Protected storage capability does not match the supplied secret store"));
    }

    auto loaded = settings_store->load();
    if (!loaded) {
        return std::unexpected(
            map_settings_error(ConfigurationErrorCode::SettingsLoadFailed, loaded.error()));
    }

    auto snapshot = ConfigurationSnapshot{
        .settings = std::move(loaded->settings),
        .settings_load_source = loaded->source,
        .settings_recovery_diagnostic = std::move(loaded->recovery_diagnostic),
        .persistent_secret_storage = std::move(secret_storage),
        .revision = 1,
        .shutting_down = false,
    };
    return ConfigurationService{std::move(settings_store), std::move(secret_store),
                                std::move(snapshot)};
}

ConfigurationSnapshot ConfigurationService::snapshot() const {
    const std::scoped_lock lock{*mutex_};
    return snapshot_;
}

std::expected<void, ConfigurationError>
ConfigurationService::save_settings(config::Settings settings) {
    const std::scoped_lock lock{*mutex_};
    if (snapshot_.shutting_down) {
        return std::unexpected(shutting_down_error(std::nullopt));
    }

    auto saved = settings_store_->save(settings);
    if (!saved) {
        return std::unexpected(
            map_settings_error(ConfigurationErrorCode::SettingsSaveFailed, saved.error()));
    }

    snapshot_.settings = std::move(settings);
    snapshot_.settings_load_source = ports::SettingsLoadSource::Primary;
    snapshot_.settings_recovery_diagnostic.reset();
    advance_revision();
    return {};
}

std::expected<support::SecretValue, ConfigurationError>
ConfigurationService::load_secret(ports::SecretId id) const {
    const std::scoped_lock lock{*mutex_};
    if (snapshot_.shutting_down) {
        return std::unexpected(shutting_down_error(id));
    }
    if (!secret_store_) {
        return std::unexpected(unavailable_secret_error(id));
    }

    auto loaded = secret_store_->load(id);
    if (!loaded) {
        return std::unexpected(
            map_secret_error(ConfigurationErrorCode::SecretLoadFailed, id, loaded.error()));
    }
    return std::move(*loaded);
}

std::expected<void, ConfigurationError>
ConfigurationService::store_secret(ports::SecretId id, const support::SecretValue& value) {
    const std::scoped_lock lock{*mutex_};
    if (snapshot_.shutting_down) {
        return std::unexpected(shutting_down_error(id));
    }
    if (!secret_store_) {
        return std::unexpected(unavailable_secret_error(id));
    }

    auto stored = secret_store_->store(id, value);
    if (!stored) {
        return std::unexpected(
            map_secret_error(ConfigurationErrorCode::SecretStoreFailed, id, stored.error()));
    }
    return {};
}

std::expected<void, ConfigurationError> ConfigurationService::erase_secret(ports::SecretId id) {
    const std::scoped_lock lock{*mutex_};
    if (snapshot_.shutting_down) {
        return std::unexpected(shutting_down_error(id));
    }
    if (!secret_store_) {
        return std::unexpected(unavailable_secret_error(id));
    }

    auto erased = secret_store_->erase(id);
    if (!erased) {
        return std::unexpected(
            map_secret_error(ConfigurationErrorCode::SecretEraseFailed, id, erased.error()));
    }
    return {};
}

void ConfigurationService::shutdown() noexcept {
    const std::scoped_lock lock{*mutex_};
    if (snapshot_.shutting_down) {
        return;
    }
    snapshot_.shutting_down = true;
    advance_revision();
}

bool ConfigurationService::is_shutting_down() const noexcept {
    const std::scoped_lock lock{*mutex_};
    return snapshot_.shutting_down;
}

ConfigurationError ConfigurationService::unavailable_secret_error(ports::SecretId id) const {
    auto error = make_error(ConfigurationErrorCode::SecretStorageUnavailable,
                            snapshot_.persistent_secret_storage.diagnostic);
    error.secret_error = snapshot_.persistent_secret_storage.error_code;
    error.secret_id = id;
    error.system_error = snapshot_.persistent_secret_storage.system_error;
    return error;
}

void ConfigurationService::advance_revision() noexcept {
    if (snapshot_.revision < std::numeric_limits<std::uint64_t>::max()) {
        ++snapshot_.revision;
    }
}

} // namespace manny_uploader::application
