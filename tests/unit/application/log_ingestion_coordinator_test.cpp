#include "manny_uploader/application/log_ingestion_coordinator.hpp"
#include "manny_uploader/filesystem/log_discovery.hpp"
#include "support/fakes.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

namespace manny_uploader::test {
namespace {

using application::CoordinatorErrorCode;
using application::LogIngestionCoordinator;
using application::MetadataParseResult;
using application::UploadCoordinator;
using domain::EncounterMetadata;
using domain::LogFileIdentity;
using domain::Provider;
using domain::ProviderSelection;
using domain::ProviderState;
using domain::UploadJobId;
using ports::MetadataParseError;
using ports::MetadataParseErrorCode;

struct IngestionFixture {
    FakeClock clock;
    FakeUploadProvider dps{Provider::DpsReport};
    FakeUploadProvider wingman{Provider::Wingman};
    FakeUploadProvider donbot{Provider::DonBot};
    FakeUploadProvider twitch{Provider::Twitch};
    FakeMetadataParser parser;
    std::array<ports::IUploadProvider*, domain::provider_count> ports{
        &dps,
        &wingman,
        &donbot,
        &twitch,
    };
};

[[nodiscard]] LogFileIdentity file(std::string name, std::uintmax_t size = 4096) {
    return LogFileIdentity{
        .canonical_path = std::filesystem::path{"logs"} / std::move(name),
        .size = size,
        .last_write_time = std::filesystem::file_time_type{} + std::chrono::seconds{7},
    };
}

[[nodiscard]] EncounterMetadata metadata() {
    return EncounterMetadata{.boss_id = 321, .pov_account = "Broadcaster.1234"};
}

[[nodiscard]] ProviderSelection providers(bool dps, bool wingman, bool donbot, bool twitch) {
    ProviderSelection selection{};
    selection[domain::provider_index(Provider::DpsReport)] = dps;
    selection[domain::provider_index(Provider::Wingman)] = wingman;
    selection[domain::provider_index(Provider::DonBot)] = donbot;
    selection[domain::provider_index(Provider::Twitch)] = twitch;
    return selection;
}

void successful_ingestion_tests(TestSuite& suite) {
    IngestionFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};

    const auto submitted =
        ingestion.submit(file("success.zevtc", 8192), providers(true, false, true, true));
    MANNY_CHECK(suite, submitted.has_value());
    MANNY_CHECK(suite, submitted.value() == UploadJobId{1});
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
    MANNY_CHECK(suite, fixture.parser.requests.front().job_id == submitted.value());
    MANNY_CHECK(suite, fixture.parser.requests.front().file.canonical_path == "logs/success.zevtc");
    MANNY_CHECK(suite, fixture.parser.requests.front().file.size == 8192);
    MANNY_CHECK(suite, fixture.dps.requests.empty());
    MANNY_CHECK(suite, fixture.donbot.requests.empty());

    auto snapshots = upload.snapshots();
    MANNY_CHECK(suite, snapshots.size() == 1);
    MANNY_CHECK(suite, !snapshots.front().encounter_metadata.has_value());
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Waiting);

    const auto parsed = ingestion.handle_result(MetadataParseResult{
        .job_id = submitted.value(),
        .metadata = metadata(),
    });
    MANNY_CHECK(suite, parsed.has_value());
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);
    MANNY_CHECK(suite, fixture.wingman.requests.empty());
    MANNY_CHECK(suite, fixture.donbot.requests.empty());
    MANNY_CHECK(suite, fixture.twitch.requests.empty());
    MANNY_CHECK(suite, fixture.dps.requests.front().metadata.boss_id == 321);
    snapshots = upload.snapshots();
    MANNY_CHECK(suite, snapshots.front().encounter_metadata.has_value());
    MANNY_CHECK(suite, snapshots.front().encounter_metadata->pov_account == "Broadcaster.1234");

    const auto duplicate = ingestion.handle_result(MetadataParseResult{
        .job_id = submitted.value(),
        .metadata = metadata(),
    });
    MANNY_CHECK(suite, !duplicate.has_value());
    MANNY_CHECK(suite, duplicate.error().code == CoordinatorErrorCode::UnexpectedResult);

    const auto unknown = ingestion.handle_result(MetadataParseResult{
        .job_id = UploadJobId{999},
        .metadata = metadata(),
    });
    MANNY_CHECK(suite, !unknown.has_value());
    MANNY_CHECK(suite, unknown.error().code == CoordinatorErrorCode::UnknownJob);
}

void parse_failure_tests(TestSuite& suite) {
    IngestionFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};

    const auto failed_job =
        ingestion.submit(file("malformed.zevtc"), providers(true, true, true, true));
    MANNY_CHECK(suite, failed_job.has_value());
    const auto failed = ingestion.handle_result(MetadataParseResult{
        .job_id = failed_job.value(),
        .metadata = std::unexpected(MetadataParseError{
            .code = MetadataParseErrorCode::MalformedLog,
            .message = "EVTC agent table is truncated",
        }),
    });
    MANNY_CHECK(suite, failed.has_value());
    MANNY_CHECK(suite, fixture.dps.requests.empty());
    MANNY_CHECK(suite, fixture.wingman.requests.empty());
    MANNY_CHECK(suite, fixture.donbot.requests.empty());

    auto snapshots = upload.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Failed);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Twitch)].state ==
                    ProviderState::Failed);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::Wingman)].detail ==
                    "EVTC agent table is truncated");

    const auto cancelled_job =
        ingestion.submit(file("cancelled.zevtc"), providers(true, false, false, false));
    MANNY_CHECK(suite, cancelled_job.has_value());
    const auto cancelled = ingestion.handle_result(MetadataParseResult{
        .job_id = cancelled_job.value(),
        .metadata = std::unexpected(MetadataParseError{
            .code = MetadataParseErrorCode::Cancelled,
            .message = "Parsing was cancelled",
        }),
    });
    MANNY_CHECK(suite, cancelled.has_value());
    snapshots = upload.snapshots();
    MANNY_CHECK(suite,
                snapshots.back().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Cancelled);
}

void dispatch_failure_and_shutdown_tests(TestSuite& suite) {
    IngestionFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};

    fixture.parser.reject_next("worker queue is closed");
    const auto rejected =
        ingestion.submit(file("queue-rejected.zevtc"), providers(true, false, false, false));
    MANNY_CHECK(suite, rejected.has_value());
    MANNY_CHECK(suite, fixture.parser.requests.empty());
    auto snapshots = upload.snapshots();
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Failed);
    MANNY_CHECK(suite,
                snapshots.front().providers[domain::provider_index(Provider::DpsReport)].detail ==
                    "Metadata parser dispatch failed: worker queue is closed");

    const auto pending =
        ingestion.submit(file("shutdown.zevtc"), providers(true, true, false, true));
    MANNY_CHECK(suite, pending.has_value());
    ingestion.cancel_all();
    ingestion.cancel_all();
    MANNY_CHECK(suite, ingestion.is_shutting_down());
    MANNY_CHECK(suite, fixture.parser.cancel_count == 1);
    MANNY_CHECK(suite, fixture.dps.cancel_count == 1);
    MANNY_CHECK(suite, fixture.wingman.cancel_count == 1);
    MANNY_CHECK(suite, fixture.donbot.cancel_count == 1);
    MANNY_CHECK(suite, fixture.twitch.cancel_count == 1);

    snapshots = upload.snapshots();
    MANNY_CHECK(suite,
                snapshots.back().providers[domain::provider_index(Provider::DpsReport)].state ==
                    ProviderState::Cancelled);
    MANNY_CHECK(suite, snapshots.back().providers[domain::provider_index(Provider::Twitch)].state ==
                           ProviderState::Cancelled);

    const auto after_shutdown =
        ingestion.submit(file("late.zevtc"), providers(true, false, false, false));
    MANNY_CHECK(suite, !after_shutdown.has_value());
    MANNY_CHECK(suite, after_shutdown.error().code == CoordinatorErrorCode::ShuttingDown);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);

    const auto late_result = ingestion.handle_result(MetadataParseResult{
        .job_id = pending.value(),
        .metadata = metadata(),
    });
    MANNY_CHECK(suite, !late_result.has_value());
    MANNY_CHECK(suite, late_result.error().code == CoordinatorErrorCode::ShuttingDown);
}

void discovery_handoff_tests(TestSuite& suite) {
    IngestionFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto discovery_created = filesystem::LogDiscoveryPipeline::create();
    MANNY_CHECK(suite, discovery_created.has_value());
    auto discovery = std::move(discovery_created.value());

    auto observation = filesystem::FileObservation{
        .canonical_path = "logs/handoff.zevtc",
        .size = 777,
        .last_write_time = std::filesystem::file_time_type{} + std::chrono::seconds{9},
    };
    const auto first = discovery.observe(observation);
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, !first->has_value());
    auto stable = discovery.observe(observation);
    MANNY_CHECK(suite, stable.has_value());
    MANNY_CHECK(suite, stable->has_value());

    const auto submitted =
        ingestion.submit(std::move(stable->value()), providers(true, false, false, false));
    MANNY_CHECK(suite, submitted.has_value());
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
    MANNY_CHECK(suite, fixture.parser.requests.front().file.size == 777);
    MANNY_CHECK(suite, fixture.parser.requests.front().file.last_write_time ==
                           observation.last_write_time);

    MANNY_CHECK(suite, discovery.observe(observation).has_value());
    const auto duplicate = discovery.observe(observation);
    MANNY_CHECK(suite, duplicate.has_value());
    MANNY_CHECK(suite, !duplicate->has_value());
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
}

} // namespace

void run_log_ingestion_coordinator_tests(TestSuite& suite) {
    successful_ingestion_tests(suite);
    parse_failure_tests(suite);
    dispatch_failure_and_shutdown_tests(suite);
    discovery_handoff_tests(suite);
}

} // namespace manny_uploader::test
