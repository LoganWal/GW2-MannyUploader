#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace manny_uploader::support {

enum class AtomicFileErrorCode : std::uint8_t {
    DirectoryCreateFailed,
    FileWriteFailed,
    FlushFailed,
    ReplaceFailed,
    DeleteFailed,
};

struct AtomicFileError {
    AtomicFileErrorCode code;
    std::string message;
    std::filesystem::path path;
};

[[nodiscard]] std::filesystem::path atomic_temporary_path(const std::filesystem::path& destination);

[[nodiscard]] std::expected<void, AtomicFileError>
write_file_atomically(const std::filesystem::path& destination,
                      std::span<const std::byte> contents);

[[nodiscard]] std::expected<void, AtomicFileError>
remove_file_if_exists(const std::filesystem::path& path);

} // namespace manny_uploader::support
