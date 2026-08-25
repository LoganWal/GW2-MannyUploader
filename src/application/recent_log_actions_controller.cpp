#include "manny_uploader/application/recent_log_actions_controller.hpp"

#include <algorithm>
#include <concepts>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manny_uploader::application {
namespace {

constexpr std::size_t maximum_command_capacity = 256;
constexpr std::size_t maximum_report_url_length = 2048;
constexpr std::string_view dps_report_prefix = "https://dps.report/";
constexpr std::string_view wingman_report_prefix = "https://gw2wingman.nevermindcreations.de/log/";
constexpr std::string_view donbot_report_prefix = "https://donbot.walmslo.com/logs/";

[[nodiscard]] RecentLogActionError make_error(RecentLogActionErrorCode code, std::string message) {
    return RecentLogActionError{
        .code = code,
        .message = std::move(message),
        .coordinator_error = std::nullopt,
        .launcher_error = std::nullopt,
    };
}

[[nodiscard]] RecentLogActionError from_coordinator_error(const CoordinatorError& error) {
    auto mapped = make_error(RecentLogActionErrorCode::RetryFailed, error.message);
    mapped.coordinator_error = error.code;
    return mapped;
}

[[nodiscard]] RecentLogActionError from_launcher_error(const ports::ExternalActionError& error) {
    auto mapped = make_error(RecentLogActionErrorCode::LaunchFailed, error.message);
    mapped.launcher_error = error.code;
    return mapped;
}

[[nodiscard]] bool trusted_report_url(std::string_view url, std::string_view prefix) noexcept {
    if (!url.starts_with(prefix) || url.size() <= prefix.size() ||
        url.size() > maximum_report_url_length) {
        return false;
    }
    return std::ranges::all_of(url, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21U && byte <= 0x7eU && character != '\\' && character != '"';
    });
}

[[nodiscard]] bool known_provider(domain::Provider provider) noexcept {
    return domain::provider_index(provider) < domain::provider_count;
}

} // namespace

std::expected<std::unique_ptr<RecentLogActionsController>, RecentLogActionError>
RecentLogActionsController::create(UploadCoordinator& uploads,
                                   ports::IExternalActionLauncher& launcher,
                                   RecentLogActionsControllerConfig config) {
    if (config.command_capacity == 0 || config.command_capacity > maximum_command_capacity ||
        config.max_commands_per_tick == 0 ||
        config.max_commands_per_tick > config.command_capacity) {
        return std::unexpected(make_error(RecentLogActionErrorCode::InvalidConfiguration,
                                          "Recent-log action limits are invalid"));
    }
    try {
        return std::unique_ptr<RecentLogActionsController>{
            new RecentLogActionsController{uploads, launcher, config}};
    } catch (...) {
        return std::unexpected(make_error(RecentLogActionErrorCode::InvalidConfiguration,
                                          "Unable to initialize recent-log actions"));
    }
}

RecentLogActionsController::RecentLogActionsController(
    UploadCoordinator& uploads, ports::IExternalActionLauncher& launcher,
    RecentLogActionsControllerConfig config) noexcept
    : uploads_{uploads}, launcher_{launcher}, config_{config} {}

std::expected<void, RecentLogActionError>
RecentLogActionsController::submit(RecentLogActionCommand command) {
    if (const auto* retry = std::get_if<RetryFailedProviderCommand>(&command);
        retry != nullptr && !known_provider(retry->provider)) {
        return std::unexpected(
            make_error(RecentLogActionErrorCode::InvalidCommand, "Retry uses an unknown provider"));
    }
    const auto id = std::visit(
        [](const auto& value) -> domain::UploadJobId {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<Value, DismissRecentLogActionErrorCommand>) {
                return {};
            } else {
                return value.job_id;
            }
        },
        command);
    if (!std::holds_alternative<DismissRecentLogActionErrorCommand>(command) && id.value == 0) {
        return std::unexpected(make_error(RecentLogActionErrorCode::InvalidCommand,
                                          "Recent-log actions require a non-zero job ID"));
    }

    std::scoped_lock lock{mutex_};
    if (published_.shutting_down) {
        return std::unexpected(make_error(RecentLogActionErrorCode::ShuttingDown,
                                          "Recent-log actions are shutting down"));
    }
    if (commands_.size() >= config_.command_capacity) {
        return std::unexpected(
            make_error(RecentLogActionErrorCode::QueueFull, "The recent-log action queue is full"));
    }
    commands_.push_back(command);
    published_.pending_commands = commands_.size();
    return {};
}

std::expected<RecentLogActionsTickReport, RecentLogActionError> RecentLogActionsController::tick() {
    std::deque<RecentLogActionCommand> pending;
    {
        std::scoped_lock lock{mutex_};
        if (published_.shutting_down) {
            return std::unexpected(make_error(RecentLogActionErrorCode::ShuttingDown,
                                              "Recent-log actions are shutting down"));
        }
        if (ticking_) {
            return std::unexpected(make_error(RecentLogActionErrorCode::Busy,
                                              "A recent-log action tick is already running"));
        }
        ticking_ = true;
        const auto count = std::min(config_.max_commands_per_tick, commands_.size());
        for (std::size_t index = 0; index < count; ++index) {
            pending.push_back(commands_.front());
            commands_.pop_front();
        }
        published_.pending_commands = commands_.size();
    }

    RecentLogActionsTickReport report;
    std::optional<RecentLogActionError> last_error;
    bool dismissed{};
    for (const auto& command : pending) {
        ++report.commands_processed;
        if (std::holds_alternative<DismissRecentLogActionErrorCommand>(command)) {
            dismissed = true;
            last_error.reset();
            continue;
        }
        try {
            if (auto executed = execute(command); !executed) {
                last_error = std::move(executed.error());
                ++report.action_failures;
            }
        } catch (...) {
            last_error = make_error(RecentLogActionErrorCode::LaunchFailed,
                                    "A recent-log action failed unexpectedly");
            ++report.action_failures;
        }
    }

    {
        std::scoped_lock lock{mutex_};
        if (last_error || dismissed) {
            published_.last_error = std::move(last_error);
        }
        ++published_.revision;
        published_.pending_commands = commands_.size();
        ticking_ = false;
    }
    return report;
}

std::expected<void, RecentLogActionError>
RecentLogActionsController::execute(const RecentLogActionCommand& command) {
    if (const auto* open_report = std::get_if<OpenDpsReportCommand>(&command)) {
        auto job = find_snapshot(open_report->job_id);
        if (!job) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnknownJob,
                                              "The selected upload job is no longer retained"));
        }
        if (!job->dps_report_result) {
            return std::unexpected(make_error(RecentLogActionErrorCode::ReportUnavailable,
                                              "This job does not have a dps.report link"));
        }
        const auto& url = job->dps_report_result->permalink;
        if (!trusted_report_url(url, dps_report_prefix)) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnsafeReportUrl,
                                              "The dps.report link is not trusted"));
        }
        if (auto opened = launcher_.open_url(url); !opened) {
            return std::unexpected(from_launcher_error(opened.error()));
        }
        return {};
    }
    if (const auto* open_report = std::get_if<OpenWingmanReportCommand>(&command)) {
        auto job = find_snapshot(open_report->job_id);
        if (!job) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnknownJob,
                                              "The selected upload job is no longer retained"));
        }
        if (!job->wingman_upload_receipt) {
            return std::unexpected(make_error(RecentLogActionErrorCode::ReportUnavailable,
                                              "This job does not have a GW2Wingman link"));
        }
        const auto& url = job->wingman_upload_receipt->permalink;
        if (!trusted_report_url(url, wingman_report_prefix)) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnsafeReportUrl,
                                              "The GW2Wingman link is not trusted"));
        }
        if (auto opened = launcher_.open_url(url); !opened) {
            return std::unexpected(from_launcher_error(opened.error()));
        }
        return {};
    }
    if (const auto* open_report = std::get_if<OpenDonBotReportCommand>(&command)) {
        auto job = find_snapshot(open_report->job_id);
        if (!job) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnknownJob,
                                              "The selected upload job is no longer retained"));
        }
        if (!job->donbot_upload_receipt || !job->donbot_upload_receipt->fight_log_id) {
            return std::unexpected(make_error(RecentLogActionErrorCode::ReportUnavailable,
                                              "This job does not have a DonBot fight link"));
        }
        const auto url = std::string{donbot_report_prefix} +
                         std::to_string(*job->donbot_upload_receipt->fight_log_id);
        if (!trusted_report_url(url, donbot_report_prefix)) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnsafeReportUrl,
                                              "The DonBot link is not trusted"));
        }
        if (auto opened = launcher_.open_url(url); !opened) {
            return std::unexpected(from_launcher_error(opened.error()));
        }
        return {};
    }
    if (const auto* open_directory = std::get_if<OpenLogDirectoryCommand>(&command)) {
        auto job = find_snapshot(open_directory->job_id);
        if (!job) {
            return std::unexpected(make_error(RecentLogActionErrorCode::UnknownJob,
                                              "The selected upload job is no longer retained"));
        }
        const auto directory = job->file.canonical_path.parent_path();
        if (directory.empty()) {
            return std::unexpected(make_error(RecentLogActionErrorCode::InvalidDirectory,
                                              "The log directory is unavailable"));
        }
        if (auto opened = launcher_.open_directory(directory); !opened) {
            return std::unexpected(from_launcher_error(opened.error()));
        }
        return {};
    }
    if (const auto* retry = std::get_if<RetryFailedProviderCommand>(&command)) {
        if (auto retried = uploads_.retry_failed_provider(retry->job_id, retry->provider);
            !retried) {
            return std::unexpected(from_coordinator_error(retried.error()));
        }
        return {};
    }
    if (const auto* reupload = std::get_if<ReuploadLogCommand>(&command)) {
        if (auto started = uploads_.reupload(reupload->job_id); !started) {
            return std::unexpected(from_coordinator_error(started.error()));
        }
        return {};
    }
    if (const auto* rechat = std::get_if<RechatLogCommand>(&command)) {
        if (auto started = uploads_.rechat(rechat->job_id); !started) {
            return std::unexpected(from_coordinator_error(started.error()));
        }
    }
    return {};
}

std::optional<UploadJobSnapshot>
RecentLogActionsController::find_snapshot(domain::UploadJobId id) const {
    auto jobs = uploads_.snapshots();
    const auto found = std::ranges::find_if(jobs, [id](const auto& job) { return job.id == id; });
    if (found == jobs.end()) {
        return std::nullopt;
    }
    return *found;
}

RecentLogActionsSnapshot RecentLogActionsController::snapshot() const {
    std::scoped_lock lock{mutex_};
    return published_;
}

void RecentLogActionsController::shutdown() noexcept {
    std::scoped_lock lock{mutex_};
    if (published_.shutting_down) {
        return;
    }
    commands_.clear();
    published_.pending_commands = 0;
    published_.accepting_commands = false;
    published_.shutting_down = true;
    ++published_.revision;
}

} // namespace manny_uploader::application
