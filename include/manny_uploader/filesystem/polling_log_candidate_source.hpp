#pragma once

#include "manny_uploader/ports/log_candidate_source.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <vector>

namespace manny_uploader::filesystem {

struct DirectorySnapshot {
    bool root_available{};
    bool complete{};
    std::vector<ports::LogFileObservation> observations;
    std::vector<std::filesystem::path> seen_candidate_paths;
    std::vector<ports::LogCandidateIssue> issues;
};

class CandidateSnapshotTracker {
  public:
    [[nodiscard]] ports::LogCandidateBatch reconcile(DirectorySnapshot snapshot);
    void clear() noexcept;

    [[nodiscard]] std::size_t retained_count() const noexcept;

  private:
    std::vector<std::filesystem::path> retained_paths_;
};

class StandardPollingLogCandidateSource final : public ports::ILogCandidateSource {
  public:
    [[nodiscard]] static std::expected<StandardPollingLogCandidateSource,
                                       ports::LogCandidateSourceError>
    create(const std::filesystem::path& root, bool recursive = true,
           std::size_t max_candidates = 4096);

    [[nodiscard]] std::expected<ports::LogCandidateBatch, ports::LogCandidateSourceError>
    poll(const std::stop_token& stop_token) override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] bool recursive() const noexcept;
    [[nodiscard]] std::size_t max_candidates() const noexcept;
    [[nodiscard]] std::size_t retained_count() const noexcept;

  private:
    StandardPollingLogCandidateSource(std::filesystem::path root, bool recursive,
                                      std::size_t max_candidates);

    [[nodiscard]] std::expected<DirectorySnapshot, ports::LogCandidateSourceError>
    scan(const std::stop_token& stop_token) const;

    std::filesystem::path root_;
    bool recursive_;
    std::size_t max_candidates_;
    CandidateSnapshotTracker tracker_;
};

} // namespace manny_uploader::filesystem
