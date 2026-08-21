#pragma once

#include "manny_uploader/config/settings.hpp"
#include "manny_uploader/ports/settings_store.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>

namespace manny_uploader::config {

inline constexpr std::size_t max_settings_file_bytes = 64U * 1024U;

using SettingsLoadSource = ports::SettingsLoadSource;
using SettingsStoreErrorCode = ports::SettingsStoreErrorCode;
using SettingsStoreError = ports::SettingsStoreError;
using SettingsLoadResult = ports::SettingsLoadResult;

class SettingsStore final : public ports::ISettingsStore {
  public:
    [[nodiscard]] static std::expected<SettingsStore, SettingsStoreError>
    create(std::filesystem::path primary_path, Settings defaults);

    [[nodiscard]] std::expected<SettingsLoadResult, SettingsStoreError> load() const override;
    [[nodiscard]] std::expected<void, SettingsStoreError>
    save(const Settings& settings) const override;

    [[nodiscard]] const std::filesystem::path& primary_path() const noexcept;
    [[nodiscard]] std::filesystem::path backup_path() const;
    [[nodiscard]] std::filesystem::path temporary_path() const;

  private:
    SettingsStore(std::filesystem::path primary_path, Settings defaults);

    std::filesystem::path primary_path_;
    Settings defaults_;
};

} // namespace manny_uploader::config
