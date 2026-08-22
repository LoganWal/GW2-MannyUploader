#include "manny_uploader/filesystem/polling_log_candidate_source.hpp"

#include "manny_uploader/filesystem/log_discovery.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace manny_uploader::filesystem {
namespace {

[[nodiscard]] ports::LogCandidateSourceError make_error(ports::LogCandidateSourceErrorCode code,
                                                        std::string message) {
    return ports::LogCandidateSourceError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::LogCandidateIssue make_issue(const std::filesystem::path& path,
                                                  std::string message) {
    return ports::LogCandidateIssue{.path = path, .message = std::move(message)};
}

void sort_and_unique(std::vector<std::filesystem::path>& paths) {
    std::ranges::sort(paths);
    const auto duplicates = std::ranges::unique(paths);
    paths.erase(duplicates.begin(), duplicates.end());
}

void normalize_snapshot(DirectorySnapshot& snapshot) {
    for (const auto& observation : snapshot.observations) {
        snapshot.seen_candidate_paths.push_back(observation.canonical_path);
    }
    sort_and_unique(snapshot.seen_candidate_paths);
    std::ranges::sort(snapshot.observations, {}, &ports::LogFileObservation::canonical_path);
    std::ranges::sort(snapshot.issues, [](const auto& left, const auto& right) {
        if (left.path != right.path) {
            return left.path < right.path;
        }
        return left.message < right.message;
    });
}

[[nodiscard]] std::expected<void, ports::LogCandidateSourceError>
inspect_entry(const std::filesystem::directory_entry& entry, DirectorySnapshot& snapshot,
              std::size_t max_candidates, const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::Cancelled,
                                          "Log directory polling cancelled"));
    }

    std::error_code error;
    const auto status = entry.symlink_status(error);
    if (error) {
        snapshot.complete = false;
        snapshot.issues.push_back(
            make_issue(entry.path(), "Unable to inspect candidate type: " + error.message()));
        return {};
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
        !is_zevtc_candidate(entry.path())) {
        return {};
    }

    auto canonical_path = std::filesystem::weakly_canonical(entry.path(), error);
    if (error) {
        snapshot.complete = false;
        snapshot.issues.push_back(
            make_issue(entry.path(), "Unable to resolve candidate path: " + error.message()));
        return {};
    }
    if (snapshot.seen_candidate_paths.size() >= max_candidates) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::ResourceLimit,
                                          "Log candidate count exceeds configured limit"));
    }
    snapshot.seen_candidate_paths.push_back(canonical_path);

    const auto size = entry.file_size(error);
    if (error) {
        snapshot.complete = false;
        snapshot.issues.push_back(
            make_issue(canonical_path, "Unable to read candidate size: " + error.message()));
        return {};
    }
    const auto last_write_time = entry.last_write_time(error);
    if (error) {
        snapshot.complete = false;
        snapshot.issues.push_back(make_issue(
            canonical_path, "Unable to read candidate modification time: " + error.message()));
        return {};
    }

    snapshot.observations.push_back(ports::LogFileObservation{
        .canonical_path = std::move(canonical_path),
        .size = size,
        .last_write_time = last_write_time,
    });
    return {};
}

template <typename Iterator>
[[nodiscard]] std::expected<void, ports::LogCandidateSourceError>
scan_entries(Iterator iterator, DirectorySnapshot& snapshot, std::size_t max_candidates,
             const std::stop_token& stop_token) {
    const Iterator end;
    while (iterator != end) {
        if (auto inspected = inspect_entry(*iterator, snapshot, max_candidates, stop_token);
            !inspected) {
            return std::unexpected(inspected.error());
        }

        const auto current_path = iterator->path();
        std::error_code error;
        iterator.increment(error);
        if (error) {
            snapshot.complete = false;
            snapshot.issues.push_back(
                make_issue(current_path, "Unable to continue directory scan: " + error.message()));
            break;
        }
    }
    return {};
}

} // namespace

ports::LogCandidateBatch CandidateSnapshotTracker::reconcile(DirectorySnapshot snapshot) {
    normalize_snapshot(snapshot);

    ports::LogCandidateBatch batch{
        .root_available = snapshot.root_available,
        .scan_complete = snapshot.complete,
        .observations = std::move(snapshot.observations),
        .removed_paths = {},
        .issues = std::move(snapshot.issues),
    };

    if (!snapshot.root_available) {
        batch.removed_paths = std::move(retained_paths_);
        retained_paths_.clear();
        return batch;
    }

    if (!snapshot.complete) {
        std::vector<std::filesystem::path> merged;
        merged.reserve(retained_paths_.size() + snapshot.seen_candidate_paths.size());
        std::ranges::set_union(retained_paths_, snapshot.seen_candidate_paths,
                               std::back_inserter(merged));
        retained_paths_ = std::move(merged);
        return batch;
    }

    std::ranges::set_difference(retained_paths_, snapshot.seen_candidate_paths,
                                std::back_inserter(batch.removed_paths));
    retained_paths_ = std::move(snapshot.seen_candidate_paths);
    return batch;
}

void CandidateSnapshotTracker::clear() noexcept {
    retained_paths_.clear();
}

std::size_t CandidateSnapshotTracker::retained_count() const noexcept {
    return retained_paths_.size();
}

std::expected<StandardPollingLogCandidateSource, ports::LogCandidateSourceError>
StandardPollingLogCandidateSource::create(const std::filesystem::path& root, bool recursive,
                                          std::size_t max_candidates) {
    if (root.empty()) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::InvalidConfiguration,
                                          "Log directory path must not be empty"));
    }
    if (max_candidates == 0) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::InvalidConfiguration,
                                          "Log candidate limit must be greater than zero"));
    }

    std::error_code error;
    auto absolute_root = std::filesystem::absolute(root, error);
    if (error) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::InvalidConfiguration,
                                          "Unable to resolve log directory: " + error.message()));
    }
    return StandardPollingLogCandidateSource{absolute_root.lexically_normal(), recursive,
                                             max_candidates};
}

StandardPollingLogCandidateSource::StandardPollingLogCandidateSource(std::filesystem::path root,
                                                                     bool recursive,
                                                                     std::size_t max_candidates)
    : root_(std::move(root)), recursive_(recursive), max_candidates_(max_candidates) {}

std::expected<void, ports::LogCandidateSourceError>
StandardPollingLogCandidateSource::reconfigure(const std::filesystem::path& root, bool recursive,
                                               std::size_t max_candidates) {
    auto replacement = create(root, recursive, max_candidates);
    if (!replacement) {
        return std::unexpected(std::move(replacement.error()));
    }

    root_ = std::move(replacement->root_);
    recursive_ = replacement->recursive_;
    max_candidates_ = replacement->max_candidates_;
    tracker_.clear();
    return {};
}

std::expected<ports::LogCandidateBatch, ports::LogCandidateSourceError>
StandardPollingLogCandidateSource::poll(const std::stop_token& stop_token) {
    auto snapshot = scan(stop_token);
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }
    return tracker_.reconcile(std::move(snapshot.value()));
}

const std::filesystem::path& StandardPollingLogCandidateSource::root() const noexcept {
    return root_;
}

bool StandardPollingLogCandidateSource::recursive() const noexcept {
    return recursive_;
}

std::size_t StandardPollingLogCandidateSource::max_candidates() const noexcept {
    return max_candidates_;
}

std::size_t StandardPollingLogCandidateSource::retained_count() const noexcept {
    return tracker_.retained_count();
}

std::expected<DirectorySnapshot, ports::LogCandidateSourceError>
StandardPollingLogCandidateSource::scan(const std::stop_token& stop_token) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::Cancelled,
                                          "Log directory polling cancelled"));
    }

    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root_, error);
    if (error == std::errc::no_such_file_or_directory) {
        return DirectorySnapshot{
            .root_available = false,
            .complete = true,
            .observations = {},
            .seen_candidate_paths = {},
            .issues = {},
        };
    }
    if (error) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::ScanFailed,
                                          "Unable to inspect log directory: " + error.message()));
    }
    if (!std::filesystem::exists(root_status)) {
        return DirectorySnapshot{
            .root_available = false,
            .complete = true,
            .observations = {},
            .seen_candidate_paths = {},
            .issues = {},
        };
    }
    if (std::filesystem::is_symlink(root_status) || !std::filesystem::is_directory(root_status)) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::ScanFailed,
                                          "Configured log path is not a regular directory"));
    }

    DirectorySnapshot snapshot{
        .root_available = true,
        .complete = true,
        .observations = {},
        .seen_candidate_paths = {},
        .issues = {},
    };
    if (recursive_) {
        std::filesystem::recursive_directory_iterator iterator{
            root_, std::filesystem::directory_options::none, error};
        if (error) {
            return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::ScanFailed,
                                              "Unable to open log directory: " + error.message()));
        }
        if (auto scanned = scan_entries(std::move(iterator), snapshot, max_candidates_, stop_token);
            !scanned) {
            return std::unexpected(scanned.error());
        }
    } else {
        std::filesystem::directory_iterator iterator{root_, error};
        if (error) {
            return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::ScanFailed,
                                              "Unable to open log directory: " + error.message()));
        }
        if (auto scanned = scan_entries(std::move(iterator), snapshot, max_candidates_, stop_token);
            !scanned) {
            return std::unexpected(scanned.error());
        }
    }

    if (stop_token.stop_requested()) {
        return std::unexpected(make_error(ports::LogCandidateSourceErrorCode::Cancelled,
                                          "Log directory polling cancelled"));
    }
    return snapshot;
}

} // namespace manny_uploader::filesystem
