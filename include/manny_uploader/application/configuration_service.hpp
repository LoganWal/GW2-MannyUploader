#pragma once

#include "manny_uploader/config/settings.hpp"
#include "manny_uploader/ports/secret_store.hpp"
#include "manny_uploader/ports/settings_store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::application {

enum class PersistentSecretStorageState : std::uint8_t {
    Available,
    UnsupportedEnvironment,
    InitializationFailed,
};

struct PersistentSecretStorageSnapshot {
    PersistentSecretStorageState state;
    std::optional<ports::SecretStoreErrorCode> error_code;
    std::string diagnostic;
    std::optional<std::uint32_t> system_error;
};

struct ConfigurationSnapshot {
    config::Settings settings;
    ports::SettingsLoadSource settings_load_source;
    std::optional<std::string> settings_recovery_diagnostic;
    PersistentSecretStorageSnapshot persistent_secret_storage;
    std::uint64_t revision;
    bool shutting_down;
};

enum class ConfigurationErrorCode : std::uint8_t {
    InvalidConfiguration,
    SettingsLoadFailed,
    SettingsSaveFailed,
    SecretStorageUnavailable,
    SecretLoadFailed,
    SecretStoreFailed,
    SecretEraseFailed,
    ShuttingDown,
};

struct ConfigurationError {
    ConfigurationErrorCode code;
    std::string message;
    std::optional<ports::SettingsStoreErrorCode> settings_error;
    std::optional<ports::SecretStoreErrorCode> secret_error;
    std::optional<ports::SecretId> secret_id;
    std::optional<std::uint32_t> system_error;
    std::optional<std::filesystem::path> settings_path;
    std::vector<config::SettingsValidationError> settings_validation_errors;
};

class ConfigurationService {
  public:
    [[nodiscard]] static std::expected<ConfigurationService, ConfigurationError>
    create(std::unique_ptr<ports::ISettingsStore> settings_store,
           std::unique_ptr<ports::ISecretStore> secret_store);

    [[nodiscard]] static std::expected<ConfigurationService, ConfigurationError>
    create_without_secret_storage(std::unique_ptr<ports::ISettingsStore> settings_store,
                                  ports::SecretStoreError unavailable_reason);

    ConfigurationService(ConfigurationService&&) noexcept = default;
    ConfigurationService& operator=(ConfigurationService&&) noexcept = default;
    ConfigurationService(const ConfigurationService&) = delete;
    ConfigurationService& operator=(const ConfigurationService&) = delete;

    [[nodiscard]] ConfigurationSnapshot snapshot() const;

    [[nodiscard]] std::expected<void, ConfigurationError> save_settings(config::Settings settings);
    [[nodiscard]] std::expected<support::SecretValue, ConfigurationError>
    load_secret(ports::SecretId id) const;
    [[nodiscard]] std::expected<void, ConfigurationError>
    store_secret(ports::SecretId id, const support::SecretValue& value);
    [[nodiscard]] std::expected<void, ConfigurationError> erase_secret(ports::SecretId id);

    void shutdown() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    ConfigurationService(std::unique_ptr<ports::ISettingsStore> settings_store,
                         std::unique_ptr<ports::ISecretStore> secret_store,
                         ConfigurationSnapshot snapshot);

    [[nodiscard]] static std::expected<ConfigurationService, ConfigurationError>
    create_impl(std::unique_ptr<ports::ISettingsStore> settings_store,
                std::unique_ptr<ports::ISecretStore> secret_store,
                PersistentSecretStorageSnapshot secret_storage);

    [[nodiscard]] ConfigurationError unavailable_secret_error(ports::SecretId id) const;
    void advance_revision() noexcept;

    std::unique_ptr<ports::ISettingsStore> settings_store_;
    std::unique_ptr<ports::ISecretStore> secret_store_;
    ConfigurationSnapshot snapshot_;
    mutable std::unique_ptr<std::mutex> mutex_{std::make_unique<std::mutex>()};
};

} // namespace manny_uploader::application
