#include "manny_uploader/filesystem/change_notifying_log_candidate_source.hpp"

#include <string>
#include <utility>

namespace manny_uploader::filesystem {
namespace {

[[nodiscard]] ports::LogCandidateSourceError invalid_configuration(std::string message) {
    return ports::LogCandidateSourceError{
        .code = ports::LogCandidateSourceErrorCode::InvalidConfiguration,
        .message = std::move(message),
    };
}

[[nodiscard]] ports::LogCandidateIssue monitor_issue(const std::filesystem::path& root,
                                                     bool disabled) {
    return ports::LogCandidateIssue{
        .path = root,
        .message = disabled ? "Native log-directory notifications failed repeatedly; using polling"
                            : "Native log-directory notification failed; polling this cycle",
    };
}

} // namespace

std::expected<ChangeNotifyingLogCandidateSource, ports::LogCandidateSourceError>
ChangeNotifyingLogCandidateSource::create(const std::filesystem::path& root, bool recursive,
                                          std::size_t max_candidates,
                                          std::unique_ptr<IDirectoryChangeMonitor> monitor,
                                          std::size_t max_consecutive_monitor_failures) {
    if (max_consecutive_monitor_failures == 0) {
        return std::unexpected(
            invalid_configuration("Native notification failure limit must be greater than zero"));
    }

    auto polling_source =
        StandardPollingLogCandidateSource::create(root, recursive, max_candidates);
    if (!polling_source) {
        return std::unexpected(std::move(polling_source.error()));
    }

    const bool monitor_requested = monitor != nullptr;
    bool monitor_enabled = monitor_requested;
    if (monitor_enabled) {
        auto configured = monitor->reconfigure(polling_source->root(), recursive);
        if (!configured) {
            monitor_enabled = false;
        }
    }

    ChangeNotifyingLogCandidateSource result{std::move(*polling_source), std::move(monitor),
                                             max_consecutive_monitor_failures, monitor_enabled};
    if (monitor_requested && !monitor_enabled) {
        result.pending_monitor_issue_ = monitor_issue(result.root(), true);
    }
    return result;
}

ChangeNotifyingLogCandidateSource::ChangeNotifyingLogCandidateSource(
    StandardPollingLogCandidateSource polling_source,
    std::unique_ptr<IDirectoryChangeMonitor> monitor, std::size_t max_consecutive_monitor_failures,
    bool monitor_enabled)
    : polling_source_{std::move(polling_source)}, monitor_{std::move(monitor)},
      max_consecutive_monitor_failures_{max_consecutive_monitor_failures},
      monitor_enabled_{monitor_enabled} {}

std::expected<void, ports::LogCandidateSourceError>
ChangeNotifyingLogCandidateSource::reconfigure(const std::filesystem::path& root, bool recursive,
                                               std::size_t max_candidates) {
    auto replacement = StandardPollingLogCandidateSource::create(root, recursive, max_candidates);
    if (!replacement) {
        return std::unexpected(std::move(replacement.error()));
    }

    monitor_enabled_ = monitor_ != nullptr;
    pending_monitor_issue_.reset();
    if (monitor_enabled_) {
        auto configured = monitor_->reconfigure(replacement->root(), recursive);
        if (!configured) {
            monitor_enabled_ = false;
            pending_monitor_issue_ = monitor_issue(replacement->root(), true);
        }
    }

    polling_source_ = std::move(*replacement);
    cached_batch_.reset();
    consecutive_monitor_failures_ = 0;
    return {};
}

std::expected<ports::LogCandidateBatch, ports::LogCandidateSourceError>
ChangeNotifyingLogCandidateSource::poll(const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return std::unexpected(ports::LogCandidateSourceError{
            .code = ports::LogCandidateSourceErrorCode::Cancelled,
            .message = "Log directory monitoring cancelled",
        });
    }

    if (!monitor_enabled_) {
        auto issue = std::move(pending_monitor_issue_);
        pending_monitor_issue_.reset();
        return scan_and_cache(issue, stop_token);
    }
    if (!cached_batch_.has_value()) {
        return scan_and_cache(std::nullopt, stop_token);
    }

    auto changed = monitor_->poll(stop_token);
    if (!changed) {
        if (changed.error().code == ports::LogCandidateSourceErrorCode::Cancelled) {
            return std::unexpected(std::move(changed.error()));
        }

        ++consecutive_monitor_failures_;
        const bool disabled = consecutive_monitor_failures_ >= max_consecutive_monitor_failures_;
        if (disabled) {
            monitor_enabled_ = false;
        }
        return scan_and_cache(monitor_issue(root(), disabled), stop_token);
    }

    consecutive_monitor_failures_ = 0;
    if (*changed == DirectoryChangeStatus::Unchanged) {
        return repeat_cached_batch(cached_batch_.value());
    }
    return scan_and_cache(std::nullopt, stop_token);
}

const std::filesystem::path& ChangeNotifyingLogCandidateSource::root() const noexcept {
    return polling_source_.root();
}

bool ChangeNotifyingLogCandidateSource::recursive() const noexcept {
    return polling_source_.recursive();
}

std::size_t ChangeNotifyingLogCandidateSource::max_candidates() const noexcept {
    return polling_source_.max_candidates();
}

bool ChangeNotifyingLogCandidateSource::using_native_notifications() const noexcept {
    return monitor_enabled_;
}

std::size_t ChangeNotifyingLogCandidateSource::consecutive_monitor_failures() const noexcept {
    return consecutive_monitor_failures_;
}

std::expected<ports::LogCandidateBatch, ports::LogCandidateSourceError>
ChangeNotifyingLogCandidateSource::scan_and_cache(
    const std::optional<ports::LogCandidateIssue>& monitor_issue,
    const std::stop_token& stop_token) {
    auto batch = polling_source_.poll(stop_token);
    if (!batch) {
        return std::unexpected(std::move(batch.error()));
    }

    if (monitor_issue.has_value()) {
        batch->issues.insert(batch->issues.begin(), *monitor_issue);
    }
    if (batch->root_available && batch->scan_complete) {
        cached_batch_ = *batch;
        cached_batch_->removed_paths.clear();
        cached_batch_->issues.clear();
    } else {
        cached_batch_.reset();
    }
    return batch;
}

ports::LogCandidateBatch ChangeNotifyingLogCandidateSource::repeat_cached_batch(
    const ports::LogCandidateBatch& cached_batch) {
    auto batch = cached_batch;
    batch.removed_paths.clear();
    batch.issues.clear();
    return batch;
}

} // namespace manny_uploader::filesystem
