#pragma once

#include "manny_uploader/config/settings.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace manny_uploader::ports {

enum class SettingsLoadSource : std::uint8_t {
    Defaults,
    Primary,
    Backup,
};

enum class SettingsStoreErrorCode : std::uint8_t {
    InvalidConfiguration,
    ValidationFailed,
    FileTooLarge,
    FileReadFailed,
    ParseFailed,
    SerializeFailed,
    DirectoryCreateFailed,
    FileWriteFailed,
    FlushFailed,
    ReplaceFailed,
    NoValidSettings,
};

struct SettingsStoreError {
    SettingsStoreErrorCode code;
    std::string message;
    std::filesystem::path path;
    std::vector<config::SettingsValidationError> validation_errors;
};

struct SettingsLoadResult {
    config::Settings settings;
    SettingsLoadSource source;
    std::optional<std::string> recovery_diagnostic;
};

class ISettingsStore {
  public:
    virtual ~ISettingsStore() = default;

    [[nodiscard]] virtual std::expected<SettingsLoadResult, SettingsStoreError> load() const = 0;
    [[nodiscard]] virtual std::expected<void, SettingsStoreError>
    save(const config::Settings& settings) const = 0;
};

} // namespace manny_uploader::ports
