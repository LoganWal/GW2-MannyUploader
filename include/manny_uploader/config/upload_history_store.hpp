#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace manny_uploader::config {

inline constexpr std::size_t default_upload_history_capacity = 10'000;

enum class UploadHistoryStoreErrorCode : std::uint8_t {
    InvalidConfiguration,
    FileReadFailed,
    FileTooLarge,
    ParseFailed,
    ValidationFailed,
    SerializeFailed,
    FileWriteFailed,
};

struct UploadHistoryStoreError {
    UploadHistoryStoreErrorCode code;
    std::string message;
    std::filesystem::path path;
};

class UploadHistoryStore {
  public:
    [[nodiscard]] static std::expected<UploadHistoryStore, UploadHistoryStoreError>
    create(std::filesystem::path path, std::size_t capacity = default_upload_history_capacity);

    [[nodiscard]] const std::vector<domain::UploadJobRecord>& records() const noexcept;
    [[nodiscard]] std::expected<void, UploadHistoryStoreError>
    merge_and_save(std::span<const domain::UploadJobRecord> updates);
    [[nodiscard]] const std::string& recovery_diagnostic() const noexcept;

  private:
    UploadHistoryStore(std::filesystem::path path, std::size_t capacity,
                       std::vector<domain::UploadJobRecord> records,
                       std::string recovery_diagnostic);

    std::filesystem::path path_;
    std::size_t capacity_;
    std::vector<domain::UploadJobRecord> records_;
    std::string recovery_diagnostic_;
};

} // namespace manny_uploader::config
