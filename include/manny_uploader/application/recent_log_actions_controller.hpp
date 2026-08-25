#pragma once

#include "manny_uploader/application/upload_coordinator.hpp"
#include "manny_uploader/ports/external_action_launcher.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace manny_uploader::application {

struct OpenDpsReportCommand {
    domain::UploadJobId job_id;
};

struct OpenWingmanReportCommand {
    domain::UploadJobId job_id;
};

struct OpenDonBotReportCommand {
    domain::UploadJobId job_id;
};

struct OpenLogDirectoryCommand {
    domain::UploadJobId job_id;
};

struct RetryFailedProviderCommand {
    domain::UploadJobId job_id;
    domain::Provider provider;
};

struct ReuploadLogCommand {
    domain::UploadJobId job_id;
};

struct RechatLogCommand {
    domain::UploadJobId job_id;
};

struct DismissRecentLogActionErrorCommand {};

using RecentLogActionCommand =
    std::variant<OpenDpsReportCommand, OpenWingmanReportCommand, OpenDonBotReportCommand,
                 OpenLogDirectoryCommand, RetryFailedProviderCommand, ReuploadLogCommand,
                 RechatLogCommand, DismissRecentLogActionErrorCommand>;

enum class RecentLogActionErrorCode : std::uint8_t {
    InvalidConfiguration,
    InvalidCommand,
    QueueFull,
    UnknownJob,
    ReportUnavailable,
    UnsafeReportUrl,
    InvalidDirectory,
    LaunchFailed,
    RetryFailed,
    Busy,
    ShuttingDown,
};

struct RecentLogActionError {
    RecentLogActionErrorCode code;
    std::string message;
    std::optional<CoordinatorErrorCode> coordinator_error;
    std::optional<ports::ExternalActionErrorCode> launcher_error;
};

struct RecentLogActionsSnapshot {
    std::optional<RecentLogActionError> last_error;
    std::size_t pending_commands{};
    std::uint64_t revision{};
    bool accepting_commands{true};
    bool shutting_down{};
};

struct RecentLogActionsTickReport {
    std::size_t commands_processed{};
    std::size_t action_failures{};
};

struct RecentLogActionsControllerConfig {
    std::size_t command_capacity{32};
    std::size_t max_commands_per_tick{8};
};

class RecentLogActionsController {
  public:
    [[nodiscard]] static std::expected<std::unique_ptr<RecentLogActionsController>,
                                       RecentLogActionError>
    create(UploadCoordinator& uploads, ports::IExternalActionLauncher& launcher,
           RecentLogActionsControllerConfig config = {});

    [[nodiscard]] std::expected<void, RecentLogActionError> submit(RecentLogActionCommand command);
    [[nodiscard]] std::expected<RecentLogActionsTickReport, RecentLogActionError> tick();
    [[nodiscard]] RecentLogActionsSnapshot snapshot() const;
    void shutdown() noexcept;

  private:
    RecentLogActionsController(UploadCoordinator& uploads, ports::IExternalActionLauncher& launcher,
                               RecentLogActionsControllerConfig config) noexcept;

    [[nodiscard]] std::expected<void, RecentLogActionError>
    execute(const RecentLogActionCommand& command);
    [[nodiscard]] std::optional<UploadJobSnapshot> find_snapshot(domain::UploadJobId id) const;

    UploadCoordinator& uploads_;
    ports::IExternalActionLauncher& launcher_;
    RecentLogActionsControllerConfig config_;
    mutable std::mutex mutex_;
    std::deque<RecentLogActionCommand> commands_;
    RecentLogActionsSnapshot published_;
    bool ticking_{};
};

} // namespace manny_uploader::application
