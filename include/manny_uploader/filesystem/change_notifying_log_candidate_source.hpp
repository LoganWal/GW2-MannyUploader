#pragma once

#include "manny_uploader/filesystem/polling_log_candidate_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

namespace manny_uploader::filesystem {

enum class DirectoryChangeStatus : std::uint8_t {
    Changed,
    Unchanged,
    Unavailable,
};

class IDirectoryChangeMonitor {
  public:
    virtual ~IDirectoryChangeMonitor() = default;

    [[nodiscard]] virtual std::expected<void, ports::LogCandidateSourceError>
    reconfigure(const std::filesystem::path& root, bool recursive) = 0;

    [[nodiscard]] virtual std::expected<DirectoryChangeStatus, ports::LogCandidateSourceError>
    poll(const std::stop_token& stop_token) = 0;
};

class ChangeNotifyingLogCandidateSource final : public ports::ILogCandidateSource {
  public:
    [[nodiscard]] static std::expected<ChangeNotifyingLogCandidateSource,
                                       ports::LogCandidateSourceError>
    create(const std::filesystem::path& root, bool recursive, std::size_t max_candidates,
           std::unique_ptr<IDirectoryChangeMonitor> monitor,
           std::size_t max_consecutive_monitor_failures = 3);

    [[nodiscard]] std::expected<void, ports::LogCandidateSourceError>
    reconfigure(const std::filesystem::path& root, bool recursive, std::size_t max_candidates);

    [[nodiscard]] std::expected<ports::LogCandidateBatch, ports::LogCandidateSourceError>
    poll(const std::stop_token& stop_token) override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] bool recursive() const noexcept;
    [[nodiscard]] std::size_t max_candidates() const noexcept;
    [[nodiscard]] bool using_native_notifications() const noexcept;
    [[nodiscard]] std::size_t consecutive_monitor_failures() const noexcept;

  private:
    ChangeNotifyingLogCandidateSource(StandardPollingLogCandidateSource polling_source,
                                      std::unique_ptr<IDirectoryChangeMonitor> monitor,
                                      std::size_t max_consecutive_monitor_failures,
                                      bool monitor_enabled);

    [[nodiscard]] std::expected<ports::LogCandidateBatch, ports::LogCandidateSourceError>
    scan_and_cache(const std::optional<ports::LogCandidateIssue>& monitor_issue,
                   const std::stop_token& stop_token);
    [[nodiscard]] static ports::LogCandidateBatch
    repeat_cached_batch(const ports::LogCandidateBatch& cached_batch);

    StandardPollingLogCandidateSource polling_source_;
    std::unique_ptr<IDirectoryChangeMonitor> monitor_;
    std::optional<ports::LogCandidateBatch> cached_batch_;
    std::optional<ports::LogCandidateIssue> pending_monitor_issue_;
    std::size_t max_consecutive_monitor_failures_;
    std::size_t consecutive_monitor_failures_{};
    bool monitor_enabled_{};
};

#if defined(_WIN32)
[[nodiscard]] std::unique_ptr<IDirectoryChangeMonitor> make_windows_directory_change_monitor();
#endif

} // namespace manny_uploader::filesystem
