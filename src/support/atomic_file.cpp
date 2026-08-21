#include "manny_uploader/support/atomic_file.hpp"

#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace manny_uploader::support {
namespace {

[[nodiscard]] AtomicFileError make_error(AtomicFileErrorCode code, std::string message,
                                         const std::filesystem::path& path) {
    return AtomicFileError{.code = code, .message = std::move(message), .path = path};
}

[[nodiscard]] std::expected<void, AtomicFileError>
ensure_parent_directory(const std::filesystem::path& destination) {
    const auto parent = destination.parent_path();
    if (parent.empty()) {
        return {};
    }

    std::error_code error;
    static_cast<void>(std::filesystem::create_directories(parent, error));
    if (error) {
        return std::unexpected(make_error(AtomicFileErrorCode::DirectoryCreateFailed,
                                          "Could not create the destination directory", parent));
    }
    return {};
}

[[nodiscard]] std::expected<void, AtomicFileError>
flush_file_to_disk(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not open the temporary file for flush", path));
    }
    const auto flushed = FlushFileBuffers(handle) != 0;
    const auto closed = CloseHandle(handle) != 0;
    if (!flushed || !closed) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not flush the temporary file", path));
    }
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not open the temporary file for flush", path));
    }
    const auto flushed = ::fsync(descriptor) == 0;
    const auto closed = ::close(descriptor) == 0;
    if (!flushed || !closed) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not flush the temporary file", path));
    }
#endif
    return {};
}

[[nodiscard]] std::expected<void, AtomicFileError>
flush_directory_to_disk(const std::filesystem::path& file_path) {
#ifdef _WIN32
    static_cast<void>(file_path);
    return {};
#else
    auto directory = file_path.parent_path();
    if (directory.empty()) {
        directory = ".";
    }
    auto flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const auto descriptor = ::open(directory.c_str(), flags);
    if (descriptor < 0) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not open the destination directory for flush",
                                          directory));
    }
    const auto flushed = ::fsync(descriptor) == 0;
    const auto closed = ::close(descriptor) == 0;
    if (!flushed || !closed) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not flush the destination directory", directory));
    }
    return {};
#endif
}

[[nodiscard]] std::expected<void, AtomicFileError>
replace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return std::unexpected(make_error(AtomicFileErrorCode::ReplaceFailed,
                                          "Could not atomically replace the destination file",
                                          destination));
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return std::unexpected(make_error(AtomicFileErrorCode::ReplaceFailed,
                                          "Could not atomically replace the destination file",
                                          destination));
    }
#endif
    return flush_directory_to_disk(destination);
}

} // namespace

std::filesystem::path atomic_temporary_path(const std::filesystem::path& destination) {
    auto temporary = destination;
    temporary += ".tmp";
    return temporary;
}

std::expected<void, AtomicFileError> write_file_atomically(const std::filesystem::path& destination,
                                                           std::span<const std::byte> contents) {
    if (contents.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(make_error(AtomicFileErrorCode::FileWriteFailed,
                                          "File contents exceed the supported stream size",
                                          destination));
    }
    auto directory = ensure_parent_directory(destination);
    if (!directory) {
        return directory;
    }

    const auto temporary = atomic_temporary_path(destination);
    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
    if (!stream.is_open()) {
        return std::unexpected(make_error(AtomicFileErrorCode::FileWriteFailed,
                                          "Could not create the temporary file", temporary));
    }
    stream.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    if (!stream) {
        return std::unexpected(make_error(AtomicFileErrorCode::FileWriteFailed,
                                          "Could not write the temporary file", temporary));
    }
    stream.flush();
    if (!stream) {
        return std::unexpected(make_error(AtomicFileErrorCode::FlushFailed,
                                          "Could not flush the temporary file", temporary));
    }
    stream.close();
    if (stream.fail()) {
        return std::unexpected(make_error(AtomicFileErrorCode::FileWriteFailed,
                                          "Could not close the temporary file", temporary));
    }

    auto flushed = flush_file_to_disk(temporary);
    if (!flushed) {
        return flushed;
    }
    return replace_file(temporary, destination);
}

std::expected<void, AtomicFileError> remove_file_if_exists(const std::filesystem::path& path) {
    std::error_code error;
    const auto removed = std::filesystem::remove(path, error);
    if (error) {
        return std::unexpected(
            make_error(AtomicFileErrorCode::DeleteFailed, "Could not remove the file", path));
    }
    if (!removed) {
        return {};
    }
    return flush_directory_to_disk(path);
}

} // namespace manny_uploader::support
