#include "manny_uploader/filesystem/log_discovery.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace manny_uploader::filesystem {
namespace {

[[nodiscard]] LogDiscoveryError make_error(LogDiscoveryErrorCode code, std::string message) {
    return LogDiscoveryError{.code = code, .message = std::move(message)};
}

template <typename Character>
[[nodiscard]] constexpr Character ascii_lower(Character value) noexcept {
    const auto upper_a = static_cast<Character>('A');
    const auto upper_z = static_cast<Character>('Z');
    if (value >= upper_a && value <= upper_z) {
        return static_cast<Character>(value + static_cast<Character>('a' - 'A'));
    }
    return value;
}

[[nodiscard]] std::expected<void, LogDiscoveryError>
validate_candidate_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(
            make_error(LogDiscoveryErrorCode::EmptyPath, "Log candidate path must not be empty"));
    }
    if (!is_zevtc_candidate(path)) {
        return std::unexpected(make_error(LogDiscoveryErrorCode::UnsupportedExtension,
                                          "Log candidate must use the .zevtc extension"));
    }
    return {};
}

} // namespace

bool is_zevtc_candidate(const std::filesystem::path& path) {
    const auto extension = path.extension().native();
    const auto expected = std::filesystem::path{".zevtc"}.native();
    return extension.size() == expected.size() &&
           std::equal(extension.begin(), extension.end(), expected.begin(),
                      [](auto left, auto right) { return ascii_lower(left) == right; });
}

std::expected<LogDedupeKey, LogDiscoveryError>
make_log_dedupe_key(const domain::LogFileIdentity& file) {
    if (const auto valid = validate_candidate_path(file.canonical_path); !valid) {
        return std::unexpected(valid.error());
    }

    return LogDedupeKey{
        .canonical_path = file.canonical_path,
        .size = file.size,
        .last_write_time = file.last_write_time,
    };
}

std::expected<FileStabilityTracker, LogDiscoveryError>
FileStabilityTracker::create(std::size_t required_matching_observations) {
    if (required_matching_observations < 2) {
        return std::unexpected(
            make_error(LogDiscoveryErrorCode::InvalidRequiredMatches,
                       "File stability requires at least two matching observations"));
    }

    return FileStabilityTracker{required_matching_observations};
}

FileStabilityTracker::FileStabilityTracker(std::size_t required_matching_observations)
    : required_matching_observations_(required_matching_observations) {}

std::expected<std::optional<domain::LogFileIdentity>, LogDiscoveryError>
FileStabilityTracker::observe(FileObservation observation) {
    if (const auto valid = validate_candidate_path(observation.canonical_path); !valid) {
        return std::unexpected(valid.error());
    }

    const auto found = pending_.find(observation.canonical_path);
    if (found == pending_.end()) {
        pending_.emplace(observation.canonical_path,
                         PendingObservation{
                             .size = observation.size,
                             .last_write_time = observation.last_write_time,
                             .matching_observations = 1,
                         });
        return std::optional<domain::LogFileIdentity>{};
    }

    auto& pending = found->second;
    if (pending.size != observation.size ||
        pending.last_write_time != observation.last_write_time) {
        pending = PendingObservation{
            .size = observation.size,
            .last_write_time = observation.last_write_time,
            .matching_observations = 1,
        };
        return std::optional<domain::LogFileIdentity>{};
    }

    ++pending.matching_observations;
    if (pending.matching_observations < required_matching_observations_) {
        return std::optional<domain::LogFileIdentity>{};
    }

    auto stable = domain::LogFileIdentity{
        .canonical_path = std::move(observation.canonical_path),
        .size = observation.size,
        .last_write_time = observation.last_write_time,
    };
    pending_.erase(found);
    return std::optional<domain::LogFileIdentity>{std::move(stable)};
}

void FileStabilityTracker::forget(const std::filesystem::path& canonical_path) {
    pending_.erase(canonical_path);
}

void FileStabilityTracker::clear() noexcept {
    pending_.clear();
}

std::expected<void, LogDiscoveryError> FileStabilityTracker::update_required_matching_observations(
    std::size_t required_matching_observations) {
    auto replacement = create(required_matching_observations);
    if (!replacement) {
        return std::unexpected(replacement.error());
    }
    *this = std::move(*replacement);
    return {};
}

std::size_t FileStabilityTracker::tracked_count() const noexcept {
    return pending_.size();
}

std::size_t FileStabilityTracker::required_matching_observations() const noexcept {
    return required_matching_observations_;
}

std::expected<LogDeduplicator, LogDiscoveryError> LogDeduplicator::create(std::size_t capacity) {
    if (capacity == 0) {
        return std::unexpected(make_error(LogDiscoveryErrorCode::InvalidDedupeCapacity,
                                          "Dedupe capacity must be greater than zero"));
    }

    return LogDeduplicator{capacity};
}

LogDeduplicator::LogDeduplicator(std::size_t capacity) : capacity_(capacity) {}

bool LogDeduplicator::remember(LogDedupeKey key) {
    if (contains(key)) {
        return false;
    }

    if (recent_.size() == capacity_) {
        recent_.pop_front();
    }
    recent_.push_back(std::move(key));
    return true;
}

bool LogDeduplicator::forget(const LogDedupeKey& key) {
    const auto found = std::find(recent_.begin(), recent_.end(), key);
    if (found == recent_.end()) {
        return false;
    }
    recent_.erase(found);
    return true;
}

bool LogDeduplicator::contains(const LogDedupeKey& key) const {
    return std::find(recent_.begin(), recent_.end(), key) != recent_.end();
}

void LogDeduplicator::clear() noexcept {
    recent_.clear();
}

std::size_t LogDeduplicator::size() const noexcept {
    return recent_.size();
}

std::size_t LogDeduplicator::capacity() const noexcept {
    return capacity_;
}

std::expected<LogDiscoveryPipeline, LogDiscoveryError>
LogDiscoveryPipeline::create(std::size_t required_matching_observations,
                             std::size_t dedupe_capacity) {
    auto stability = FileStabilityTracker::create(required_matching_observations);
    if (!stability) {
        return std::unexpected(stability.error());
    }

    auto deduplicator = LogDeduplicator::create(dedupe_capacity);
    if (!deduplicator) {
        return std::unexpected(deduplicator.error());
    }

    return LogDiscoveryPipeline{std::move(stability.value()), std::move(deduplicator.value())};
}

LogDiscoveryPipeline::LogDiscoveryPipeline(FileStabilityTracker stability,
                                           LogDeduplicator deduplicator)
    : stability_(std::move(stability)), deduplicator_(std::move(deduplicator)) {}

std::expected<std::optional<domain::LogFileIdentity>, LogDiscoveryError>
LogDiscoveryPipeline::observe(FileObservation observation) {
    auto stable = stability_.observe(std::move(observation));
    if (!stable) {
        return std::unexpected(stable.error());
    }
    if (!stable->has_value()) {
        return std::optional<domain::LogFileIdentity>{};
    }

    auto key = make_log_dedupe_key(stable->value());
    if (!key) {
        return std::unexpected(key.error());
    }
    if (!deduplicator_.remember(std::move(key.value()))) {
        return std::optional<domain::LogFileIdentity>{};
    }

    return std::optional<domain::LogFileIdentity>{std::move(stable->value())};
}

void LogDiscoveryPipeline::forget(const std::filesystem::path& canonical_path) {
    stability_.forget(canonical_path);
}

std::expected<void, LogDiscoveryError>
LogDiscoveryPipeline::release(const domain::LogFileIdentity& file) {
    auto key = make_log_dedupe_key(file);
    if (!key) {
        return std::unexpected(key.error());
    }
    static_cast<void>(deduplicator_.forget(key.value()));
    return {};
}

std::expected<bool, LogDiscoveryError>
LogDiscoveryPipeline::remember(const domain::LogFileIdentity& file) {
    auto key = make_log_dedupe_key(file);
    if (!key) {
        return std::unexpected(key.error());
    }
    return deduplicator_.remember(std::move(*key));
}

void LogDiscoveryPipeline::clear() noexcept {
    stability_.clear();
    deduplicator_.clear();
}

void LogDiscoveryPipeline::clear_pending() noexcept {
    stability_.clear();
}

std::expected<void, LogDiscoveryError> LogDiscoveryPipeline::update_required_matching_observations(
    std::size_t required_matching_observations) {
    return stability_.update_required_matching_observations(required_matching_observations);
}

std::size_t LogDiscoveryPipeline::tracked_count() const noexcept {
    return stability_.tracked_count();
}

std::size_t LogDiscoveryPipeline::dedupe_size() const noexcept {
    return deduplicator_.size();
}

std::size_t LogDiscoveryPipeline::required_matching_observations() const noexcept {
    return stability_.required_matching_observations();
}

} // namespace manny_uploader::filesystem
