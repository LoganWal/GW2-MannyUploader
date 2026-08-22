#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/log_candidate_source.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace manny_uploader::filesystem {

enum class LogDiscoveryErrorCode : std::uint8_t {
    InvalidRequiredMatches,
    InvalidDedupeCapacity,
    EmptyPath,
    UnsupportedExtension,
};

struct LogDiscoveryError {
    LogDiscoveryErrorCode code;
    std::string message;
};

using FileObservation = ports::LogFileObservation;

struct LogDedupeKey {
    std::filesystem::path canonical_path;
    std::uintmax_t size{};
    std::filesystem::file_time_type last_write_time{};

    [[nodiscard]] friend bool operator==(const LogDedupeKey&,
                                         const LogDedupeKey&) noexcept = default;
};

[[nodiscard]] bool is_zevtc_candidate(const std::filesystem::path& path);

[[nodiscard]] std::expected<LogDedupeKey, LogDiscoveryError>
make_log_dedupe_key(const domain::LogFileIdentity& file);

class FileStabilityTracker {
  public:
    [[nodiscard]] static std::expected<FileStabilityTracker, LogDiscoveryError>
    create(std::size_t required_matching_observations = 2);

    [[nodiscard]] std::expected<std::optional<domain::LogFileIdentity>, LogDiscoveryError>
    observe(FileObservation observation);

    void forget(const std::filesystem::path& canonical_path);
    void clear() noexcept;

    [[nodiscard]] std::expected<void, LogDiscoveryError>
    update_required_matching_observations(std::size_t required_matching_observations);

    [[nodiscard]] std::size_t tracked_count() const noexcept;
    [[nodiscard]] std::size_t required_matching_observations() const noexcept;

  private:
    struct PendingObservation {
        std::uintmax_t size{};
        std::filesystem::file_time_type last_write_time{};
        std::size_t matching_observations{};
    };

    explicit FileStabilityTracker(std::size_t required_matching_observations);

    std::size_t required_matching_observations_;
    std::unordered_map<std::filesystem::path, PendingObservation> pending_;
};

class LogDeduplicator {
  public:
    [[nodiscard]] static std::expected<LogDeduplicator, LogDiscoveryError>
    create(std::size_t capacity);

    [[nodiscard]] bool remember(LogDedupeKey key);
    [[nodiscard]] bool forget(const LogDedupeKey& key);
    [[nodiscard]] bool contains(const LogDedupeKey& key) const;
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

  private:
    explicit LogDeduplicator(std::size_t capacity);

    std::size_t capacity_;
    std::deque<LogDedupeKey> recent_;
};

class LogDiscoveryPipeline {
  public:
    [[nodiscard]] static std::expected<LogDiscoveryPipeline, LogDiscoveryError>
    create(std::size_t required_matching_observations = 2, std::size_t dedupe_capacity = 256);

    [[nodiscard]] std::expected<std::optional<domain::LogFileIdentity>, LogDiscoveryError>
    observe(FileObservation observation);

    void forget(const std::filesystem::path& canonical_path);
    [[nodiscard]] std::expected<void, LogDiscoveryError>
    release(const domain::LogFileIdentity& file);
    [[nodiscard]] std::expected<bool, LogDiscoveryError>
    remember(const domain::LogFileIdentity& file);
    void clear() noexcept;
    void clear_pending() noexcept;
    [[nodiscard]] std::expected<void, LogDiscoveryError>
    update_required_matching_observations(std::size_t required_matching_observations);

    [[nodiscard]] std::size_t tracked_count() const noexcept;
    [[nodiscard]] std::size_t dedupe_size() const noexcept;
    [[nodiscard]] std::size_t required_matching_observations() const noexcept;

  private:
    LogDiscoveryPipeline(FileStabilityTracker stability, LogDeduplicator deduplicator);

    FileStabilityTracker stability_;
    LogDeduplicator deduplicator_;
};

} // namespace manny_uploader::filesystem
