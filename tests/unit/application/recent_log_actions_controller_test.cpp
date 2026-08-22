#include "manny_uploader/application/recent_log_actions_controller.hpp"
#include "support/fakes.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace manny_uploader::test {
namespace {

class FakeExternalActionLauncher final : public ports::IExternalActionLauncher {
  public:
    std::expected<void, ports::ExternalActionError> open_url(std::string_view url) override {
        urls.emplace_back(url);
        return next_result();
    }

    std::expected<void, ports::ExternalActionError>
    open_directory(const std::filesystem::path& directory) override {
        directories.push_back(directory);
        return next_result();
    }

    std::expected<void, ports::ExternalActionError> next_result() {
        if (!failure) {
            return {};
        }
        auto result = std::unexpected(*failure);
        failure.reset();
        return result;
    }

    std::vector<std::string> urls;
    std::vector<std::filesystem::path> directories;
    std::optional<ports::ExternalActionError> failure;
};

struct Fixture {
    FakeClock clock;
    FakeUploadProvider dps{domain::Provider::DpsReport};
    FakeUploadProvider wingman{domain::Provider::Wingman};
    FakeUploadProvider donbot{domain::Provider::DonBot};
    FakeUploadProvider twitch{domain::Provider::Twitch};
    std::array<ports::IUploadProvider*, domain::provider_count> providers{&dps, &wingman, &donbot,
                                                                          &twitch};
    FakeExternalActionLauncher launcher;
};

[[nodiscard]] domain::LogFileIdentity log_file(std::string name = "actions.zevtc") {
    return domain::LogFileIdentity{
        .canonical_path = std::filesystem::path{"/logs/encounters"} / std::move(name),
        .size = 4096,
        .last_write_time = {},
    };
}

[[nodiscard]] domain::ProviderSelection selection(bool wingman = false) {
    domain::ProviderSelection result{};
    result[domain::provider_index(domain::Provider::DpsReport)] = true;
    result[domain::provider_index(domain::Provider::Wingman)] = wingman;
    return result;
}

[[nodiscard]] ports::UploadResult dps_success(domain::UploadJobId id, std::string url) {
    return ports::UploadResult{
        .job_id = id,
        .provider = domain::Provider::DpsReport,
        .outcome = ports::UploadOutcome::Succeeded,
        .detail = "uploaded",
        .retry_after = std::nullopt,
        .dps_report_result =
            domain::DpsReportResult{
                .permalink = std::move(url),
                .encounter_name = "Cerus",
                .boss_id = 26257,
                .mode = "CM",
                .success = true,
            },
    };
}

void open_and_validation_tests(TestSuite& suite) {
    Fixture fixture;
    auto uploads = application::UploadCoordinator::create(fixture.clock, fixture.providers);
    MANNY_CHECK(suite, uploads.has_value());
    auto job = uploads->add_job(
        log_file(), domain::EncounterMetadata{.boss_id = 26257, .pov_account = {}}, selection());
    MANNY_CHECK(suite, job.has_value());
    MANNY_CHECK(
        suite, uploads->handle_result(dps_success(*job, "https://dps.report/abc-123")).has_value());
    auto controller = application::RecentLogActionsController::create(*uploads, fixture.launcher);
    MANNY_CHECK(suite, controller.has_value());
    MANNY_CHECK(
        suite,
        (*controller)->submit(application::OpenDpsReportCommand{.job_id = *job}).has_value());
    MANNY_CHECK(
        suite,
        (*controller)->submit(application::OpenLogDirectoryCommand{.job_id = *job}).has_value());
    const auto report = (*controller)->tick();
    MANNY_CHECK(suite, report.has_value() && report->commands_processed == 2);
    MANNY_CHECK(suite,
                fixture.launcher.urls == std::vector<std::string>{"https://dps.report/abc-123"});
    MANNY_CHECK(suite, fixture.launcher.directories ==
                           std::vector<std::filesystem::path>{"/logs/encounters"});

    auto unsafe_job =
        uploads->add_job(log_file("unsafe.zevtc"),
                         domain::EncounterMetadata{.boss_id = 1, .pov_account = {}}, selection());
    MANNY_CHECK(suite, unsafe_job.has_value());
    MANNY_CHECK(
        suite,
        uploads->handle_result(dps_success(*unsafe_job, "https://dps.report.evil/x")).has_value());
    MANNY_CHECK(suite, (*controller)
                           ->submit(application::OpenDpsReportCommand{.job_id = *unsafe_job})
                           .has_value());
    MANNY_CHECK(suite, (*controller)->tick()->action_failures == 1);
    MANNY_CHECK(suite, (*controller)->snapshot().last_error->code ==
                           application::RecentLogActionErrorCode::UnsafeReportUrl);
    MANNY_CHECK(suite, fixture.launcher.urls.size() == 1);

    MANNY_CHECK(
        suite,
        (*controller)->submit(application::DismissRecentLogActionErrorCommand{}).has_value());
    MANNY_CHECK(suite, (*controller)->tick().has_value());
    MANNY_CHECK(suite, !(*controller)->snapshot().last_error.has_value());

    fixture.launcher.failure = ports::ExternalActionError{
        .code = ports::ExternalActionErrorCode::LaunchFailed,
        .message = "launcher failed",
    };
    MANNY_CHECK(
        suite,
        (*controller)->submit(application::OpenLogDirectoryCommand{.job_id = *job}).has_value());
    MANNY_CHECK(suite, (*controller)->tick()->action_failures == 1);
    MANNY_CHECK(suite, (*controller)->snapshot().last_error->code ==
                           application::RecentLogActionErrorCode::LaunchFailed);
    MANNY_CHECK(suite, (*controller)->snapshot().last_error->launcher_error ==
                           ports::ExternalActionErrorCode::LaunchFailed);

    MANNY_CHECK(suite, (*controller)
                           ->submit(application::OpenLogDirectoryCommand{
                               .job_id = domain::UploadJobId{999}})
                           .has_value());
    MANNY_CHECK(suite, (*controller)->tick()->action_failures == 1);
    MANNY_CHECK(suite, (*controller)->snapshot().last_error->code ==
                           application::RecentLogActionErrorCode::UnknownJob);
    const auto invalid_id =
        (*controller)
            ->submit(application::OpenLogDirectoryCommand{.job_id = domain::UploadJobId{}});
    MANNY_CHECK(suite, !invalid_id.has_value());
    MANNY_CHECK(suite,
                invalid_id.error().code == application::RecentLogActionErrorCode::InvalidCommand);
}

void retry_queue_and_shutdown_tests(TestSuite& suite) {
    Fixture fixture;
    auto uploads = application::UploadCoordinator::create(fixture.clock, fixture.providers);
    MANNY_CHECK(suite, uploads.has_value());
    const auto invalid_controller = application::RecentLogActionsController::create(
        *uploads, fixture.launcher,
        application::RecentLogActionsControllerConfig{.command_capacity = 0,
                                                      .max_commands_per_tick = 1});
    MANNY_CHECK(suite, !invalid_controller.has_value());
    auto job = uploads->add_job(
        log_file(), domain::EncounterMetadata{.boss_id = 1, .pov_account = {}}, selection(true));
    MANNY_CHECK(suite, job.has_value());
    MANNY_CHECK(suite, uploads
                           ->handle_result(ports::UploadResult{
                               .job_id = *job,
                               .provider = domain::Provider::Wingman,
                               .outcome = ports::UploadOutcome::Failed,
                               .detail = "failed",
                               .retry_after = std::nullopt,
                               .dps_report_result = std::nullopt,
                           })
                           .has_value());
    auto controller = application::RecentLogActionsController::create(
        *uploads, fixture.launcher,
        application::RecentLogActionsControllerConfig{.command_capacity = 1,
                                                      .max_commands_per_tick = 1});
    MANNY_CHECK(suite, controller.has_value());
    MANNY_CHECK(suite, (*controller)
                           ->submit(application::RetryFailedProviderCommand{
                               .job_id = *job, .provider = domain::Provider::Wingman})
                           .has_value());
    const auto full = (*controller)->submit(application::OpenLogDirectoryCommand{.job_id = *job});
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().code == application::RecentLogActionErrorCode::QueueFull);
    MANNY_CHECK(suite, (*controller)->tick()->action_failures == 0);
    MANNY_CHECK(suite, fixture.wingman.requests.size() == 2);
    MANNY_CHECK(suite, fixture.wingman.requests.back().user_initiated_retry);

    (*controller)->shutdown();
    (*controller)->shutdown();
    const auto stopped =
        (*controller)->submit(application::OpenLogDirectoryCommand{.job_id = *job});
    MANNY_CHECK(suite, !stopped.has_value());
    MANNY_CHECK(suite, stopped.error().code == application::RecentLogActionErrorCode::ShuttingDown);
}

} // namespace

void run_recent_log_actions_controller_tests(TestSuite& suite) {
    open_and_validation_tests(suite);
    retry_queue_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
