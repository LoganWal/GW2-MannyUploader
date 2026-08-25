#include "manny_uploader/application/upload_coordinator.hpp"
#include "support/fakes.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace manny_uploader::test {
namespace {

using application::CoordinatorErrorCode;
using application::UploadCoordinator;
using application::UploadOutcome;
using application::UploadResult;
using domain::DpsReportResult;
using domain::EncounterMetadata;
using domain::LogFileIdentity;
using domain::Provider;
using domain::ProviderSelection;
using domain::ProviderState;
using domain::UploadJobId;

struct CoordinatorFixture {
    FakeClock clock;
    FakeUploadProvider dps{Provider::DpsReport};
    FakeUploadProvider wingman{Provider::Wingman};
    FakeUploadProvider donbot{Provider::DonBot};
    FakeUploadProvider twitch{Provider::Twitch};
    std::array<ports::IUploadProvider*, domain::provider_count> ports{
        &dps,
        &wingman,
        &donbot,
        &twitch,
    };
};

[[nodiscard]] LogFileIdentity file(std::string name) {
    return LogFileIdentity{
        .canonical_path = std::filesystem::path{"logs"} / std::move(name),
        .size = 4096,
        .last_write_time = {},
    };
}

[[nodiscard]] EncounterMetadata metadata() {
    return EncounterMetadata{.boss_id = 123, .pov_account = "Broadcaster.1234"};
}

[[nodiscard]] ProviderSelection providers(bool dps, bool wingman, bool donbot, bool twitch) {
    ProviderSelection selection{};
    selection[domain::provider_index(Provider::DpsReport)] = dps;
    selection[domain::provider_index(Provider::Wingman)] = wingman;
    selection[domain::provider_index(Provider::DonBot)] = donbot;
    selection[domain::provider_index(Provider::Twitch)] = twitch;
    return selection;
}

[[nodiscard]] DpsReportResult dps_result(std::string permalink = "https://dps.report/example") {
    return DpsReportResult{
        .permalink = std::move(permalink),
        .encounter_name = "Example Encounter",
        .mode = "CM",
        .success = true,
    };
}

void creation_and_dispatch_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(created.value());

    const auto first =
        coordinator.add_job(file("first.zevtc"), metadata(), providers(true, true, true, true));
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, first.value() == UploadJobId{1});
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);
    MANNY_CHECK(suite, fixture.wingman.requests.size() == 1);
    MANNY_CHECK(suite, fixture.donbot.requests.size() == 1);
    MANNY_CHECK(suite, fixture.twitch.requests.empty());
    MANNY_CHECK(suite, fixture.dps.requests.front().attempt == 1);
    MANNY_CHECK(suite, fixture.dps.requests.front().job_id == first.value());
    MANNY_CHECK(suite, fixture.dps.requests.front().metadata.pov_account == "Broadcaster.1234");

    fixture.clock.advance(std::chrono::seconds{5});
    const auto second =
        coordinator.add_job(file("second.zevtc"), metadata(), providers(true, false, false, false));
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, second.value() == UploadJobId{2});

    const auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite, snapshots.size() == 2);
    MANNY_CHECK(suite, snapshots[0].detected_at == std::chrono::system_clock::time_point{});
    MANNY_CHECK(suite, snapshots[1].detected_at ==
                           std::chrono::system_clock::time_point{} + std::chrono::seconds{5});
}

void pending_metadata_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(created.value());

    const auto pending =
        coordinator.add_pending_job(file("pending.zevtc"), providers(true, true, false, true));
    MANNY_CHECK(suite, pending.has_value());
    MANNY_CHECK(suite, fixture.dps.requests.empty());
    MANNY_CHECK(suite, fixture.wingman.requests.empty());
    auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite, !snapshots.front().encounter_metadata.has_value());
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Waiting);

    const auto started = coordinator.start_pending_job(pending.value(), metadata());
    MANNY_CHECK(suite, started.has_value());
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);
    MANNY_CHECK(suite, fixture.wingman.requests.size() == 1);
    MANNY_CHECK(suite, fixture.donbot.requests.empty());
    MANNY_CHECK(suite, fixture.twitch.requests.empty());
    snapshots = coordinator.snapshots();
    MANNY_CHECK(suite, snapshots.front().encounter_metadata.has_value());

    const auto duplicate_metadata = coordinator.start_pending_job(pending.value(), metadata());
    MANNY_CHECK(suite, !duplicate_metadata.has_value());
    MANNY_CHECK(suite, duplicate_metadata.error().code == CoordinatorErrorCode::UnexpectedResult);

    const auto unknown = coordinator.start_pending_job(UploadJobId{999}, metadata());
    MANNY_CHECK(suite, !unknown.has_value());
    MANNY_CHECK(suite, unknown.error().code == CoordinatorErrorCode::UnknownJob);

    const auto failed =
        coordinator.add_pending_job(file("parse-failed.zevtc"), providers(true, false, true, true));
    MANNY_CHECK(suite, failed.has_value());
    MANNY_CHECK(suite, coordinator.fail_pending_job(failed.value(), "invalid archive").has_value());
    snapshots = coordinator.snapshots();
    const auto& failed_snapshot = snapshots.back();
    MANNY_CHECK(suite,
                failed_snapshot.providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Failed);
    MANNY_CHECK(suite, failed_snapshot.providers[domain::provider_index(Provider::Wingman)].state ==
                           ProviderState::Disabled);
    MANNY_CHECK(suite, failed_snapshot.providers[domain::provider_index(Provider::DonBot)].state ==
                           ProviderState::Failed);
    MANNY_CHECK(suite, failed_snapshot.providers[domain::provider_index(Provider::Twitch)].state ==
                           ProviderState::Failed);
    MANNY_CHECK(suite,
                failed_snapshot.providers[domain::provider_index(Provider::DpsReport)].detail ==
                    "invalid archive");

    const auto late_metadata = coordinator.start_pending_job(failed.value(), metadata());
    MANNY_CHECK(suite, !late_metadata.has_value());
    MANNY_CHECK(suite, late_metadata.error().code == CoordinatorErrorCode::UnexpectedResult);

    const auto cancelled = coordinator.add_pending_job(file("parse-cancelled.zevtc"),
                                                       providers(true, false, false, false));
    MANNY_CHECK(suite, cancelled.has_value());
    MANNY_CHECK(suite, coordinator.cancel_pending_job(cancelled.value(), "cancelled").has_value());
    snapshots = coordinator.snapshots();
    MANNY_CHECK(suite,
                snapshots.back().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Cancelled);
}

void result_correlation_and_twitch_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(created.value());

    const auto job_id =
        coordinator.add_job(file("result.zevtc"), metadata(), providers(true, true, false, true));
    MANNY_CHECK(suite, job_id.has_value());

    const auto unknown = coordinator.handle_result(UploadResult{
        .job_id = UploadJobId{999},
        .provider = Provider::Wingman,
        .outcome = UploadOutcome::Succeeded,
        .detail = "accepted",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
        .wingman_upload_receipt =
            domain::WingmanUploadReceipt{
                .permalink = "https://gw2wingman.nevermindcreations.de/log/example",
            },
    });
    MANNY_CHECK(suite, !unknown.has_value());
    MANNY_CHECK(suite, unknown.error().code == CoordinatorErrorCode::UnknownJob);

    const auto wingman = coordinator.handle_result(UploadResult{
        .job_id = job_id.value(),
        .provider = Provider::Wingman,
        .outcome = UploadOutcome::Succeeded,
        .detail = "accepted",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
        .wingman_upload_receipt =
            domain::WingmanUploadReceipt{
                .permalink = "https://gw2wingman.nevermindcreations.de/log/example",
            },
    });
    MANNY_CHECK(suite, wingman.has_value());
    MANNY_CHECK(suite, fixture.twitch.requests.empty());

    const auto dps = coordinator.handle_result(UploadResult{
        .job_id = job_id.value(),
        .provider = Provider::DpsReport,
        .outcome = UploadOutcome::Succeeded,
        .detail = "uploaded",
        .retry_after = std::nullopt,
        .dps_report_result = dps_result(),
    });
    MANNY_CHECK(suite, dps.has_value());
    MANNY_CHECK(suite, fixture.twitch.requests.size() == 1);
    MANNY_CHECK(suite, fixture.twitch.requests.front().job_id == job_id.value());
    MANNY_CHECK(suite, fixture.twitch.requests.front().dps_report_result.has_value());
    MANNY_CHECK(suite, fixture.twitch.requests.front().dps_report_result->permalink ==
                           "https://dps.report/example");

    const auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite, snapshots.size() == 1);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Succeeded);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Wingman)].state ==
                    ProviderState::Succeeded);
    MANNY_CHECK(suite, snapshots.front().wingman_upload_receipt.has_value());
    MANNY_CHECK(suite, snapshots.front().wingman_upload_receipt &&
                           snapshots.front().wingman_upload_receipt->permalink ==
                               "https://gw2wingman.nevermindcreations.de/log/example");
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Twitch)].state ==
                    ProviderState::Active);

    const auto twitch = coordinator.handle_result(UploadResult{
        .job_id = job_id.value(),
        .provider = Provider::Twitch,
        .outcome = UploadOutcome::Succeeded,
        .detail = "posted",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
        .twitch_delivery_receipt =
            domain::TwitchDeliveryReceipt{
                .status = domain::TwitchDeliveryStatus::Sent,
                .message_id = "message-123",
            },
    });
    MANNY_CHECK(suite, twitch.has_value());
    const auto completed = coordinator.snapshots();
    MANNY_CHECK(suite, completed.front().twitch_delivery_receipt.has_value());
    MANNY_CHECK(suite, completed.front().twitch_delivery_receipt->message_id == "message-123");
    MANNY_CHECK(suite,
                completed.front().providers[domain::provider_index(Provider::Twitch)].state ==
                    ProviderState::Succeeded);
}

void retry_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(created.value());

    const auto job_id =
        coordinator.add_job(file("retry.zevtc"), metadata(), providers(true, false, false, false));
    MANNY_CHECK(suite, job_id.has_value());

    for (const auto invalid_delay : {std::chrono::seconds{0}, std::chrono::seconds{25 * 60 * 60}}) {
        const auto invalid = coordinator.handle_result(UploadResult{
            .job_id = job_id.value(),
            .provider = Provider::DpsReport,
            .outcome = UploadOutcome::Retry,
            .detail = "invalid retry",
            .retry_after = invalid_delay,
            .dps_report_result = std::nullopt,
        });
        MANNY_CHECK(suite, !invalid.has_value());
        MANNY_CHECK(suite, invalid.error().code == CoordinatorErrorCode::UnexpectedResult);
    }
    const auto missing_delay = coordinator.handle_result(UploadResult{
        .job_id = job_id.value(),
        .provider = Provider::DpsReport,
        .outcome = UploadOutcome::Retry,
        .detail = "missing retry",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
    });
    MANNY_CHECK(suite, !missing_delay.has_value());

    const auto retry = coordinator.handle_result(UploadResult{
        .job_id = job_id.value(),
        .provider = Provider::DpsReport,
        .outcome = UploadOutcome::Retry,
        .detail = "rate limited",
        .retry_after = std::chrono::seconds{30},
        .dps_report_result = std::nullopt,
    });
    MANNY_CHECK(suite, retry.has_value());
    auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].retry_at ==
                    fixture.clock.steady_now() + std::chrono::seconds{30});

    fixture.clock.advance(std::chrono::seconds{29});
    MANNY_CHECK(suite, coordinator.dispatch_due_retries() == 0);
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);

    fixture.clock.advance(std::chrono::seconds{1});
    MANNY_CHECK(suite, coordinator.dispatch_due_retries() == 1);
    MANNY_CHECK(suite, fixture.dps.requests.size() == 2);
    MANNY_CHECK(suite, fixture.dps.requests.back().attempt == 2);

    CoordinatorFixture dependency_fixture;
    auto dependency_created =
        UploadCoordinator::create(dependency_fixture.clock, dependency_fixture.ports);
    MANNY_CHECK(suite, dependency_created.has_value());
    auto dependency_coordinator = std::move(*dependency_created);
    const auto dependency_job = dependency_coordinator.add_job(
        file("wingman-retry.zevtc"), metadata(), providers(true, true, false, false));
    MANNY_CHECK(suite, dependency_job.has_value());
    MANNY_CHECK(suite, dependency_coordinator
                           .handle_result(UploadResult{
                               .job_id = *dependency_job,
                               .provider = Provider::DpsReport,
                               .outcome = UploadOutcome::Succeeded,
                               .detail = "uploaded",
                               .retry_after = std::nullopt,
                               .dps_report_result = dps_result(),
                           })
                           .has_value());
    MANNY_CHECK(suite, dependency_coordinator
                           .handle_result(UploadResult{
                               .job_id = *dependency_job,
                               .provider = Provider::Wingman,
                               .outcome = UploadOutcome::Retry,
                               .detail = "retry",
                               .retry_after = std::chrono::seconds{1},
                               .dps_report_result = std::nullopt,
                           })
                           .has_value());
    dependency_fixture.clock.advance(std::chrono::seconds{1});
    MANNY_CHECK(suite, dependency_coordinator.dispatch_due_retries() == 1);
    MANNY_CHECK(suite, dependency_fixture.wingman.requests.size() == 2);
    MANNY_CHECK(suite, !dependency_fixture.wingman.requests.back().dps_report_result.has_value());
}

void provider_result_drain_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(*created);
    const auto job_id =
        coordinator.add_job(file("drain.zevtc"), metadata(), providers(true, false, false, false));
    MANNY_CHECK(suite, job_id.has_value());

    fixture.dps.results.push_back(UploadResult{
        .job_id = *job_id,
        .provider = Provider::DpsReport,
        .outcome = UploadOutcome::Succeeded,
        .detail = "uploaded",
        .retry_after = std::nullopt,
        .dps_report_result = dps_result("https://dps.report/drained"),
    });
    const auto none = coordinator.drain_provider_results(0);
    MANNY_CHECK(suite, none.has_value());
    MANNY_CHECK(suite, *none == 0);
    MANNY_CHECK(suite, fixture.dps.results.size() == 1);

    const auto drained = coordinator.drain_provider_results(1);
    MANNY_CHECK(suite, drained.has_value());
    MANNY_CHECK(suite, *drained == 1);
    MANNY_CHECK(suite, fixture.dps.results.empty());
    const auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite, snapshots.front().dps_report_result.has_value());
    MANNY_CHECK(suite,
                snapshots.front().dps_report_result->permalink == "https://dps.report/drained");
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Succeeded);

    CoordinatorFixture fair_fixture;
    auto fair_created = UploadCoordinator::create(fair_fixture.clock, fair_fixture.ports);
    MANNY_CHECK(suite, fair_created.has_value());
    auto fair = std::move(*fair_created);
    const auto fair_job =
        fair.add_job(file("fair-drain.zevtc"), metadata(), providers(true, true, false, false));
    MANNY_CHECK(suite, fair_job.has_value());
    fair_fixture.dps.results.push_back(UploadResult{
        .job_id = *fair_job,
        .provider = Provider::DpsReport,
        .outcome = UploadOutcome::Succeeded,
        .detail = "uploaded",
        .retry_after = std::nullopt,
        .dps_report_result = dps_result(),
    });
    fair_fixture.wingman.results.push_back(UploadResult{
        .job_id = *fair_job,
        .provider = Provider::Wingman,
        .outcome = UploadOutcome::Succeeded,
        .detail = "accepted",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
    });
    MANNY_CHECK(suite, fair.drain_provider_results(1) == 1);
    MANNY_CHECK(suite, fair_fixture.wingman.results.size() == 1);
    MANNY_CHECK(suite, fair.drain_provider_results(1) == 1);
    MANNY_CHECK(suite, fair_fixture.wingman.results.empty());
    const auto fair_snapshot = fair.snapshots();
    MANNY_CHECK(
        suite, fair_snapshot.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                   ProviderState::Succeeded);
    MANNY_CHECK(suite,
                fair_snapshot.front().providers[domain::provider_index(Provider::Wingman)].state ==
                    ProviderState::Succeeded);
}

void failure_and_capacity_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports, 1);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(created.value());

    fixture.wingman.reject_next("queue closed");
    const auto first =
        coordinator.add_job(file("failed.zevtc"), metadata(), providers(true, true, false, true));
    MANNY_CHECK(suite, first.has_value());
    auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Wingman)].state ==
                    ProviderState::Failed);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Wingman)].detail ==
                    "queue closed");

    const auto full = coordinator.add_job(file("blocked.zevtc"), metadata(),
                                          providers(true, false, false, false));
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().code == CoordinatorErrorCode::CapacityReached);

    const auto dps_failed = coordinator.handle_result(UploadResult{
        .job_id = first.value(),
        .provider = Provider::DpsReport,
        .outcome = UploadOutcome::Failed,
        .detail = "permanent failure",
        .retry_after = std::nullopt,
        .dps_report_result = std::nullopt,
    });
    MANNY_CHECK(suite, dps_failed.has_value());
    snapshots = coordinator.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Twitch)].state ==
                    ProviderState::Skipped);

    const auto replacement = coordinator.add_job(file("replacement.zevtc"), metadata(),
                                                 providers(true, false, false, false));
    MANNY_CHECK(suite, replacement.has_value());
    MANNY_CHECK(suite, replacement.value() == UploadJobId{2});
    MANNY_CHECK(suite, coordinator.snapshots().front().id == replacement.value());
}

void live_history_limit_reconfiguration_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports, 3);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(*created);
    MANNY_CHECK(suite, coordinator.history_limit() == 3);

    std::array<UploadJobId, 3> jobs{};
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        const auto added = coordinator.add_job(file("history-" + std::to_string(index) + ".zevtc"),
                                               metadata(), providers(true, false, false, false));
        MANNY_CHECK(suite, added.has_value());
        jobs[index] = *added;
        MANNY_CHECK(suite, coordinator
                               .handle_result(UploadResult{
                                   .job_id = *added,
                                   .provider = Provider::DpsReport,
                                   .outcome = UploadOutcome::Failed,
                                   .detail = "settled",
                                   .retry_after = std::nullopt,
                                   .dps_report_result = std::nullopt,
                               })
                               .has_value());
    }

    const auto invalid = coordinator.update_history_limit(0);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == CoordinatorErrorCode::InvalidHistoryLimit);
    MANNY_CHECK(suite, coordinator.history_limit() == 3);

    MANNY_CHECK(suite, coordinator.update_history_limit(1).has_value());
    MANNY_CHECK(suite, coordinator.history_limit() == 1);
    const auto trimmed = coordinator.snapshots();
    MANNY_CHECK(suite, trimmed.size() == 1);
    MANNY_CHECK(suite, trimmed.front().id == jobs.back());

    MANNY_CHECK(suite, coordinator.update_history_limit(2).has_value());
    const auto active_one = coordinator.add_job(file("active-one.zevtc"), metadata(),
                                                providers(true, false, false, false));
    MANNY_CHECK(suite, active_one.has_value());
    MANNY_CHECK(suite, coordinator.update_history_limit(1).has_value());
    MANNY_CHECK(suite, coordinator.snapshots().size() == 1);
    MANNY_CHECK(suite, coordinator.snapshots().front().id == *active_one);

    MANNY_CHECK(suite, coordinator.update_history_limit(2).has_value());
    const auto active_two = coordinator.add_job(file("active-two.zevtc"), metadata(),
                                                providers(true, false, false, false));
    MANNY_CHECK(suite, active_two.has_value());
    MANNY_CHECK(suite, coordinator.update_history_limit(1).has_value());
    MANNY_CHECK(suite, coordinator.snapshots().size() == 2);
    MANNY_CHECK(suite, !coordinator
                            .add_job(file("over-active-limit.zevtc"), metadata(),
                                     providers(true, false, false, false))
                            .has_value());
    MANNY_CHECK(suite, coordinator
                           .handle_result(UploadResult{
                               .job_id = *active_one,
                               .provider = Provider::DpsReport,
                               .outcome = UploadOutcome::Failed,
                               .detail = "settled after resize",
                               .retry_after = std::nullopt,
                               .dps_report_result = std::nullopt,
                           })
                           .has_value());
    const auto after_settlement = coordinator.snapshots();
    MANNY_CHECK(suite, after_settlement.size() == 1);
    MANNY_CHECK(suite, after_settlement.front().id == *active_two);

    coordinator.cancel_all();
    const auto stopped = coordinator.update_history_limit(2);
    MANNY_CHECK(suite, !stopped.has_value());
    MANNY_CHECK(suite, stopped.error().code == CoordinatorErrorCode::ShuttingDown);
}

void manual_retry_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(*created);
    const auto job = coordinator.add_job(file("manual-retry.zevtc"), metadata(),
                                         providers(true, true, false, true));
    MANNY_CHECK(suite, job.has_value());

    MANNY_CHECK(suite, coordinator
                           .handle_result(UploadResult{
                               .job_id = *job,
                               .provider = Provider::Wingman,
                               .outcome = UploadOutcome::Failed,
                               .detail = "failed",
                               .retry_after = std::nullopt,
                               .dps_report_result = std::nullopt,
                           })
                           .has_value());
    MANNY_CHECK(suite, coordinator.retry_failed_provider(*job, Provider::Wingman).has_value());
    MANNY_CHECK(suite, fixture.wingman.requests.size() == 2);
    MANNY_CHECK(suite, fixture.wingman.requests.back().attempt == 2);
    MANNY_CHECK(suite, fixture.wingman.requests.back().user_initiated_retry);
    const auto active_retry = coordinator.retry_failed_provider(*job, Provider::Wingman);
    MANNY_CHECK(suite, !active_retry.has_value());
    MANNY_CHECK(suite, active_retry.error().code == CoordinatorErrorCode::DomainRuleViolation);

    MANNY_CHECK(suite, coordinator
                           .handle_result(UploadResult{
                               .job_id = *job,
                               .provider = Provider::DpsReport,
                               .outcome = UploadOutcome::Failed,
                               .detail = "failed",
                               .retry_after = std::nullopt,
                               .dps_report_result = std::nullopt,
                           })
                           .has_value());
    MANNY_CHECK(suite, coordinator.retry_failed_provider(*job, Provider::DpsReport).has_value());
    auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Twitch)].state ==
                    ProviderState::Waiting);
    MANNY_CHECK(suite, fixture.dps.requests.back().user_initiated_retry);
    MANNY_CHECK(suite, coordinator
                           .handle_result(UploadResult{
                               .job_id = *job,
                               .provider = Provider::DpsReport,
                               .outcome = UploadOutcome::Succeeded,
                               .detail = "uploaded",
                               .retry_after = std::nullopt,
                               .dps_report_result = dps_result(),
                           })
                           .has_value());
    MANNY_CHECK(suite, fixture.twitch.requests.size() == 1);
    MANNY_CHECK(suite, !fixture.twitch.requests.back().user_initiated_retry);

    MANNY_CHECK(suite, coordinator
                           .handle_result(UploadResult{
                               .job_id = *job,
                               .provider = Provider::Twitch,
                               .outcome = UploadOutcome::Failed,
                               .detail = "held",
                               .retry_after = std::nullopt,
                               .dps_report_result = std::nullopt,
                               .twitch_delivery_receipt =
                                   domain::TwitchDeliveryReceipt{
                                       .status = domain::TwitchDeliveryStatus::AutoMod,
                                       .message_id = std::nullopt,
                                   },
                           })
                           .has_value());
    MANNY_CHECK(suite, coordinator.retry_failed_provider(*job, Provider::Twitch).has_value());
    MANNY_CHECK(suite, fixture.twitch.requests.size() == 2);
    MANNY_CHECK(suite, fixture.twitch.requests.back().user_initiated_retry);
    MANNY_CHECK(suite, !coordinator.snapshots().front().twitch_delivery_receipt.has_value());

    const auto pending = coordinator.add_pending_job(file("parse-failure.zevtc"),
                                                     providers(true, false, false, false));
    MANNY_CHECK(suite, pending.has_value());
    MANNY_CHECK(suite, coordinator.fail_pending_job(*pending, "invalid archive").has_value());
    const auto parse_retry = coordinator.retry_failed_provider(*pending, Provider::DpsReport);
    MANNY_CHECK(suite, !parse_retry.has_value());
    MANNY_CHECK(suite, parse_retry.error().code == CoordinatorErrorCode::UnexpectedResult);

    CoordinatorFixture rejected_fixture;
    auto rejected_created =
        UploadCoordinator::create(rejected_fixture.clock, rejected_fixture.ports);
    MANNY_CHECK(suite, rejected_created.has_value());
    auto rejected = std::move(*rejected_created);
    const auto rejected_job = rejected.add_job(file("rejected-retry.zevtc"), metadata(),
                                               providers(true, false, false, true));
    MANNY_CHECK(suite, rejected_job.has_value());
    MANNY_CHECK(suite, rejected
                           .handle_result(UploadResult{
                               .job_id = *rejected_job,
                               .provider = Provider::DpsReport,
                               .outcome = UploadOutcome::Failed,
                               .detail = "failed",
                               .retry_after = std::nullopt,
                               .dps_report_result = std::nullopt,
                           })
                           .has_value());
    rejected_fixture.dps.reject_next("queue full");
    const auto rejected_retry = rejected.retry_failed_provider(*rejected_job, Provider::DpsReport);
    MANNY_CHECK(suite, !rejected_retry.has_value());
    const auto rejected_snapshot = rejected.snapshots();
    MANNY_CHECK(
        suite,
        rejected_snapshot.front().providers[domain::provider_index(Provider::DpsReport)].state ==
            ProviderState::Failed);
    MANNY_CHECK(
        suite,
        rejected_snapshot.front().providers[domain::provider_index(Provider::Twitch)].state ==
            ProviderState::Skipped);
}

[[nodiscard]] domain::UploadJobRecord completed_record(std::string name) {
    std::array<domain::ProviderStatus, domain::provider_count> statuses{};
    for (auto& status : statuses) {
        status = domain::ProviderStatus{
            .state = ProviderState::Succeeded,
            .attempts = 1,
            .detail = "completed",
            .retry_at = std::nullopt,
        };
    }
    return domain::UploadJobRecord{
        .file = file(std::move(name)),
        .detected_at = std::chrono::system_clock::time_point{std::chrono::seconds{42}},
        .encounter_metadata = metadata(),
        .dps_report_result = dps_result(),
        .wingman_upload_receipt =
            domain::WingmanUploadReceipt{
                .permalink = "https://gw2wingman.nevermindcreations.de/log/restored",
            },
        .donbot_upload_receipt =
            domain::DonBotUploadReceipt{
                .upload_id = 81,
                .fight_log_id = 91,
            },
        .twitch_delivery_receipt =
            domain::TwitchDeliveryReceipt{
                .status = domain::TwitchDeliveryStatus::Sent,
                .message_id = "message-1",
            },
        .providers = std::move(statuses),
    };
}

void persistent_restore_and_explicit_delivery_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(*created);
    const std::array restored{completed_record("restored.zevtc")};
    MANNY_CHECK(suite, coordinator.restore_history(restored).has_value());
    MANNY_CHECK(suite, fixture.dps.requests.empty());
    MANNY_CHECK(suite, fixture.wingman.requests.empty());
    MANNY_CHECK(suite, fixture.donbot.requests.empty());
    MANNY_CHECK(suite, fixture.twitch.requests.empty());

    const auto restored_id = coordinator.snapshots().front().id;
    MANNY_CHECK(suite, coordinator.reupload(restored_id).has_value());
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);
    MANNY_CHECK(suite, fixture.wingman.requests.size() == 1);
    MANNY_CHECK(suite, fixture.donbot.requests.size() == 1);
    MANNY_CHECK(suite, fixture.twitch.requests.empty());
    MANNY_CHECK(suite, fixture.dps.requests.front().user_initiated_retry);
    MANNY_CHECK(suite, fixture.wingman.requests.front().user_initiated_retry);
    MANNY_CHECK(suite, fixture.donbot.requests.front().user_initiated_retry);
    const auto after_reupload = coordinator.snapshots().front();
    MANNY_CHECK(suite, !after_reupload.dps_report_result.has_value());
    MANNY_CHECK(suite, !after_reupload.wingman_upload_receipt.has_value());
    MANNY_CHECK(suite, !after_reupload.donbot_upload_receipt.has_value());
    MANNY_CHECK(suite, after_reupload.twitch_delivery_receipt.has_value());
    MANNY_CHECK(suite, !coordinator.reupload(restored_id).has_value());

    CoordinatorFixture twitch_fixture;
    auto twitch_created = UploadCoordinator::create(twitch_fixture.clock, twitch_fixture.ports);
    MANNY_CHECK(suite, twitch_created.has_value());
    auto twitch_coordinator = std::move(*twitch_created);
    MANNY_CHECK(suite, twitch_coordinator.restore_history(restored).has_value());
    const auto twitch_id = twitch_coordinator.snapshots().front().id;
    MANNY_CHECK(suite, twitch_coordinator.rechat(twitch_id).has_value());
    MANNY_CHECK(suite, twitch_fixture.twitch.requests.size() == 1);
    MANNY_CHECK(suite, twitch_fixture.twitch.requests.front().user_initiated_retry);
    MANNY_CHECK(suite, twitch_fixture.twitch.requests.front().dps_report_result.has_value());
    MANNY_CHECK(suite, !twitch_coordinator.snapshots().front().twitch_delivery_receipt.has_value());

    CoordinatorFixture interrupted_fixture;
    auto interrupted_created =
        UploadCoordinator::create(interrupted_fixture.clock, interrupted_fixture.ports);
    MANNY_CHECK(suite, interrupted_created.has_value());
    auto interrupted = std::move(*interrupted_created);
    auto interrupted_record = completed_record("interrupted.zevtc");
    interrupted_record.providers[domain::provider_index(Provider::DpsReport)].state =
        ProviderState::Waiting;
    interrupted_record.providers[domain::provider_index(Provider::Wingman)].state =
        ProviderState::Active;
    interrupted_record.providers[domain::provider_index(Provider::DonBot)].state =
        ProviderState::RetryScheduled;
    interrupted_record.providers[domain::provider_index(Provider::DonBot)].retry_at =
        std::chrono::steady_clock::time_point{std::chrono::seconds{30}};
    MANNY_CHECK(suite, interrupted.restore_history(std::array{interrupted_record}).has_value());
    const auto interrupted_snapshot = interrupted.snapshots().front();
    for (const auto provider : {Provider::DpsReport, Provider::Wingman, Provider::DonBot}) {
        const auto& status = interrupted_snapshot.providers[domain::provider_index(provider)];
        MANNY_CHECK(suite, status.state == ProviderState::Failed);
        MANNY_CHECK(suite, status.detail == "Interrupted by the previous game session");
        MANNY_CHECK(suite, !status.retry_at.has_value());
    }
    MANNY_CHECK(suite, interrupted_fixture.dps.requests.empty());
    MANNY_CHECK(suite, interrupted_fixture.wingman.requests.empty());
    MANNY_CHECK(suite, interrupted_fixture.donbot.requests.empty());
    MANNY_CHECK(suite, interrupted_fixture.twitch.requests.empty());
}

void validation_and_cancellation_tests(TestSuite& suite) {
    CoordinatorFixture fixture;
    std::array<ports::IUploadProvider*, 1> duplicate_seed{&fixture.dps};
    auto duplicate_ports = std::array<ports::IUploadProvider*, 2>{&fixture.dps, &fixture.dps};

    const auto invalid_limit = UploadCoordinator::create(fixture.clock, duplicate_seed, 0);
    MANNY_CHECK(suite, !invalid_limit.has_value());
    MANNY_CHECK(suite, invalid_limit.error().code == CoordinatorErrorCode::InvalidHistoryLimit);

    const auto duplicate = UploadCoordinator::create(fixture.clock, duplicate_ports);
    MANNY_CHECK(suite, !duplicate.has_value());
    MANNY_CHECK(suite, duplicate.error().code == CoordinatorErrorCode::DuplicateProvider);

    std::array<ports::IUploadProvider*, 1> only_dps{&fixture.dps};
    auto partial = UploadCoordinator::create(fixture.clock, only_dps);
    MANNY_CHECK(suite, partial.has_value());
    auto partial_coordinator = std::move(partial.value());
    const auto missing = partial_coordinator.add_job(file("missing.zevtc"), metadata(),
                                                     providers(true, true, false, false));
    MANNY_CHECK(suite, !missing.has_value());
    MANNY_CHECK(suite, missing.error().code == CoordinatorErrorCode::MissingProvider);

    auto created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, created.has_value());
    auto coordinator = std::move(created.value());
    const auto job_id =
        coordinator.add_job(file("cancel.zevtc"), metadata(), providers(true, true, true, true));
    MANNY_CHECK(suite, job_id.has_value());

    coordinator.cancel_all();
    coordinator.cancel_all();
    MANNY_CHECK(suite, coordinator.is_shutting_down());
    MANNY_CHECK(suite, fixture.dps.cancel_count == 1);
    MANNY_CHECK(suite, fixture.wingman.cancel_count == 1);
    MANNY_CHECK(suite, fixture.donbot.cancel_count == 1);
    MANNY_CHECK(suite, fixture.twitch.cancel_count == 1);

    const auto snapshots = coordinator.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Cancelled);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Twitch)].state ==
                    ProviderState::Cancelled);

    const auto after_cancel =
        coordinator.add_job(file("late.zevtc"), metadata(), providers(true, false, false, false));
    MANNY_CHECK(suite, !after_cancel.has_value());
    MANNY_CHECK(suite, after_cancel.error().code == CoordinatorErrorCode::ShuttingDown);
}

} // namespace

void run_upload_coordinator_tests(TestSuite& suite) {
    creation_and_dispatch_tests(suite);
    pending_metadata_tests(suite);
    result_correlation_and_twitch_tests(suite);
    retry_tests(suite);
    provider_result_drain_tests(suite);
    failure_and_capacity_tests(suite);
    live_history_limit_reconfiguration_tests(suite);
    manual_retry_tests(suite);
    persistent_restore_and_explicit_delivery_tests(suite);
    validation_and_cancellation_tests(suite);
}

} // namespace manny_uploader::test
