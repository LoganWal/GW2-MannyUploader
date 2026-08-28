#include "manny_uploader/config/settings_store.hpp"

#include "manny_uploader/support/atomic_file.hpp"

#include <glaze/json.hpp>

#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace manny_uploader::config {
namespace {

struct SettingsReadOptions : glz::opts {
    bool validate_trailing_whitespace{true};
    bool linear_search{true};
};

struct SettingsWriteOptions : glz::opts {
    bool prettify{true};
    bool escape_control_characters{true};
    bool linear_search{true};
    char indentation_char{' '};
    std::uint8_t indentation_width{2};
};

struct DecodedFile {
    Settings settings;
    std::string contents;
};

[[nodiscard]] SettingsStoreError make_error(SettingsStoreErrorCode code, std::string message,
                                            const std::filesystem::path& path) {
    return SettingsStoreError{
        .code = code,
        .message = std::move(message),
        .path = path,
        .validation_errors = {},
    };
}

[[nodiscard]] SettingsStoreError
make_validation_error(std::string message, const std::filesystem::path& path,
                      std::vector<SettingsValidationError> errors) {
    return SettingsStoreError{
        .code = SettingsStoreErrorCode::ValidationFailed,
        .message = std::move(message),
        .path = path,
        .validation_errors = std::move(errors),
    };
}

[[nodiscard]] std::filesystem::path with_suffix(const std::filesystem::path& path,
                                                std::string_view suffix) {
    auto result = path;
    result += suffix;
    return result;
}

[[nodiscard]] std::span<const std::byte> string_bytes(std::string_view value) noexcept {
    return std::as_bytes(std::span{value.data(), value.size()});
}

[[nodiscard]] SettingsStoreError map_atomic_error(const support::AtomicFileError& error) {
    auto code = SettingsStoreErrorCode::FileWriteFailed;
    switch (error.code) {
    case support::AtomicFileErrorCode::DirectoryCreateFailed:
        code = SettingsStoreErrorCode::DirectoryCreateFailed;
        break;
    case support::AtomicFileErrorCode::FileWriteFailed:
        code = SettingsStoreErrorCode::FileWriteFailed;
        break;
    case support::AtomicFileErrorCode::FlushFailed:
        code = SettingsStoreErrorCode::FlushFailed;
        break;
    case support::AtomicFileErrorCode::ReplaceFailed:
        code = SettingsStoreErrorCode::ReplaceFailed;
        break;
    case support::AtomicFileErrorCode::DeleteFailed:
        code = SettingsStoreErrorCode::FileWriteFailed;
        break;
    }
    return make_error(code, error.message, error.path);
}

[[nodiscard]] std::expected<bool, SettingsStoreError>
file_exists(const std::filesystem::path& path) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileReadFailed,
                                          "Could not inspect the settings file", path));
    }
    return exists;
}

[[nodiscard]] std::expected<std::string, SettingsStoreError>
read_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileReadFailed,
                                          "Could not determine the settings file size", path));
    }
    if (file_size > max_settings_file_bytes) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileTooLarge,
                                          "Settings file exceeds the 64 KiB limit", path));
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileReadFailed,
                                          "Could not open the settings file", path));
    }

    std::string contents(static_cast<std::size_t>(file_size), '\0');
    if (!contents.empty()) {
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
            return std::unexpected(make_error(SettingsStoreErrorCode::FileReadFailed,
                                              "Settings file changed while it was being read",
                                              path));
        }
    }

    char extra{};
    stream.read(&extra, 1);
    if (stream.gcount() != 0) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileReadFailed,
                                          "Settings file changed while it was being read", path));
    }
    if (!stream.eof()) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileReadFailed,
                                          "Could not finish reading the settings file", path));
    }
    return contents;
}

[[nodiscard]] std::expected<Settings, SettingsStoreError>
decode_settings(std::string_view document, const std::filesystem::path& path) {
    Settings settings;
    settings.schema_version = 0;

    const auto parse_error = glz::read<SettingsReadOptions{}>(settings, document);
    if (parse_error) {
        return std::unexpected(
            make_error(SettingsStoreErrorCode::ParseFailed,
                       "Settings JSON is invalid: " + glz::format_error(parse_error), path));
    }

    // Older settings persisted a channel ID without recording whether the user explicitly chose
    // it. Treat those IDs as stale so loading an older file cannot silently override guild routes.
    if (!settings.donbot.enabled || !settings.donbot.discord_channel_override_explicit) {
        settings.donbot.discord_channel_override_explicit = false;
        settings.donbot.selected_discord_channel_id.clear();
    }

    auto validation_errors = validate_settings(settings);
    if (!validation_errors.empty()) {
        return std::unexpected(make_validation_error("Settings values failed validation", path,
                                                     std::move(validation_errors)));
    }
    return settings;
}

[[nodiscard]] std::expected<DecodedFile, SettingsStoreError>
load_file(const std::filesystem::path& path) {
    auto contents = read_file(path);
    if (!contents) {
        return std::unexpected(std::move(contents.error()));
    }
    auto settings = decode_settings(*contents, path);
    if (!settings) {
        return std::unexpected(std::move(settings.error()));
    }
    return DecodedFile{.settings = std::move(*settings), .contents = std::move(*contents)};
}

[[nodiscard]] std::expected<std::string, SettingsStoreError>
encode_settings(const Settings& settings, const std::filesystem::path& path) {
    std::string document;
    const auto write_error = glz::write<SettingsWriteOptions{}>(settings, document);
    if (write_error) {
        return std::unexpected(
            make_error(SettingsStoreErrorCode::SerializeFailed,
                       "Could not serialize settings: " + glz::format_error(write_error), path));
    }
    document.push_back('\n');
    if (document.size() > max_settings_file_bytes) {
        return std::unexpected(make_error(SettingsStoreErrorCode::FileTooLarge,
                                          "Serialized settings exceed the 64 KiB limit", path));
    }
    return document;
}

[[nodiscard]] bool may_overwrite_invalid_primary(SettingsStoreErrorCode code) noexcept {
    return code == SettingsStoreErrorCode::FileTooLarge ||
           code == SettingsStoreErrorCode::ParseFailed ||
           code == SettingsStoreErrorCode::ValidationFailed;
}

} // namespace

SettingsStore::SettingsStore(std::filesystem::path primary_path, Settings defaults)
    : primary_path_{std::move(primary_path)}, defaults_{std::move(defaults)} {}

std::expected<SettingsStore, SettingsStoreError>
SettingsStore::create(std::filesystem::path primary_path, Settings defaults) {
    const auto filename = primary_path.filename();
    if (primary_path.empty() || filename.empty() || filename == "." || filename == "..") {
        return std::unexpected(make_error(SettingsStoreErrorCode::InvalidConfiguration,
                                          "Settings path must name a file", primary_path));
    }
    auto validation_errors = validate_settings(defaults);
    if (!validation_errors.empty()) {
        return std::unexpected(make_validation_error("Default settings failed validation",
                                                     primary_path, std::move(validation_errors)));
    }
    return SettingsStore{std::move(primary_path), std::move(defaults)};
}

std::expected<SettingsLoadResult, SettingsStoreError> SettingsStore::load() const {
    const auto primary_exists = file_exists(primary_path_);
    if (!primary_exists) {
        return std::unexpected(primary_exists.error());
    }

    std::optional<SettingsStoreError> primary_failure;
    if (*primary_exists) {
        auto primary = load_file(primary_path_);
        if (primary) {
            return SettingsLoadResult{
                .settings = std::move(primary->settings),
                .source = SettingsLoadSource::Primary,
                .recovery_diagnostic = std::nullopt,
            };
        }
        primary_failure = std::move(primary.error());
    }

    const auto backup = backup_path();
    const auto backup_exists = file_exists(backup);
    if (!backup_exists) {
        return std::unexpected(backup_exists.error());
    }
    if (*backup_exists) {
        auto loaded_backup = load_file(backup);
        if (loaded_backup) {
            const auto diagnostic = primary_failure
                                        ? "Primary settings failed: " + primary_failure->message +
                                              "; loaded the last-known-good backup"
                                        : "Primary settings file is missing; loaded the "
                                          "last-known-good backup";
            return SettingsLoadResult{
                .settings = std::move(loaded_backup->settings),
                .source = SettingsLoadSource::Backup,
                .recovery_diagnostic = diagnostic,
            };
        }

        auto message = primary_failure ? "Primary settings failed: " + primary_failure->message
                                       : "Primary settings file is missing";
        message += "; backup settings failed: " + loaded_backup.error().message;
        return std::unexpected(
            make_error(SettingsStoreErrorCode::NoValidSettings, std::move(message), primary_path_));
    }

    if (primary_failure) {
        return std::unexpected(make_error(SettingsStoreErrorCode::NoValidSettings,
                                          "Primary settings failed: " + primary_failure->message +
                                              "; no backup exists",
                                          primary_path_));
    }

    return SettingsLoadResult{
        .settings = defaults_,
        .source = SettingsLoadSource::Defaults,
        .recovery_diagnostic = std::nullopt,
    };
}

std::expected<void, SettingsStoreError> SettingsStore::save(const Settings& settings) const {
    auto validation_errors = validate_settings(settings);
    if (!validation_errors.empty()) {
        return std::unexpected(make_validation_error("Settings values failed validation",
                                                     primary_path_, std::move(validation_errors)));
    }

    auto document = encode_settings(settings, primary_path_);
    if (!document) {
        return std::unexpected(std::move(document.error()));
    }

    const auto primary_exists = file_exists(primary_path_);
    if (!primary_exists) {
        return std::unexpected(primary_exists.error());
    }
    if (*primary_exists) {
        auto previous = load_file(primary_path_);
        if (previous) {
            auto backup_result =
                support::write_file_atomically(backup_path(), string_bytes(previous->contents));
            if (!backup_result) {
                return std::unexpected(map_atomic_error(backup_result.error()));
            }
        } else if (!may_overwrite_invalid_primary(previous.error().code)) {
            return std::unexpected(std::move(previous.error()));
        }
    }

    auto written = support::write_file_atomically(primary_path_, string_bytes(*document));
    if (!written) {
        return std::unexpected(map_atomic_error(written.error()));
    }
    return {};
}

const std::filesystem::path& SettingsStore::primary_path() const noexcept {
    return primary_path_;
}

std::filesystem::path SettingsStore::backup_path() const {
    return with_suffix(primary_path_, ".bak");
}

std::filesystem::path SettingsStore::temporary_path() const {
    return support::atomic_temporary_path(primary_path_);
}

} // namespace manny_uploader::config
