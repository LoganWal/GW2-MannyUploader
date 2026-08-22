#include "manny_uploader/application/application_pump.hpp"
#include "support/fakes.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

using application::ApplicationPump;
using application::ApplicationPumpConfig;
using application::ApplicationPumpErrorCode;
using application::LogIngestionCoordinator;
using application::UploadCoordinator;
using domain::Provider;
using ports::LogCandidateBatch;
using ports::LogCandidateSourceError;
using ports::LogCandidateSourceErrorCode;

class FakeCandidateSource final : public ports::ILogCandidateSource {
  public:
    [[nodiscard]] std::expected<LogCandidateBatch, LogCandidateSourceError>
    poll(const std::stop_token&) override {
        ++poll_count;
        if (polls.empty()) {
            return LogCandidateBatch{
                .root_available = true,
                .scan_complete = true,
                .observations = {},
                .removed_paths = {},
                .issues = {},
            };
        }
        auto result = std::move(polls.front());
        polls.pop_front();
        return result;
    }

    void push(LogCandidateBatch batch) {
        polls.emplace_back(std::move(batch));
    }

    void fail(LogCandidateSourceErrorCode code, std::string message) {
        polls.emplace_back(
            std::unexpected(LogCandidateSourceError{.code = code, .message = std::move(message)}));
    }

    std::deque<std::expected<LogCandidateBatch, LogCandidateSourceError>> polls;
    std::size_t poll_count{};
};

struct PumpFixture {
    FakeClock clock;
    FakeUploadProvider dps{Provider::DpsReport};
    FakeUploadProvider wingman{Provider::Wingman};
    FakeUploadProvider donbot{Provider::DonBot};
    FakeUploadProvider twitch{Provider::Twitch};
    FakeMetadataParser parser;
    FakeCandidateSource source;
    std::array<ports::IUploadProvider*, domain::provider_count> provider_ports{
        &dps,
        &wingman,
        &donbot,
        &twitch,
    };
};

[[nodiscard]] ports::LogFileObservation observation(std::string path, std::uintmax_t size = 100,
                                                    std::int64_t written_at = 1) {
    return ports::LogFileObservation{
        .canonical_path = std::move(path),
        .size = size,
        .last_write_time = std::filesystem::file_time_type{} + std::chrono::seconds{written_at},
    };
}

[[nodiscard]] LogCandidateBatch batch(std::vector<ports::LogFileObservation> observations = {},
                                      std::vector<std::filesystem::path> removed = {},
                                      bool root_available = true, bool complete = true,
                                      std::vector<ports::LogCandidateIssue> issues = {}) {
    return LogCandidateBatch{
        .root_available = root_available,
        .scan_complete = complete,
        .observations = std::move(observations),
        .removed_paths = std::move(removed),
        .issues = std::move(issues),
    };
}

[[nodiscard]] domain::ProviderSelection dps_only() {
    domain::ProviderSelection selection{};
    selection[domain::provider_index(Provider::DpsReport)] = true;
    return selection;
}

[[nodiscard]] domain::EncounterMetadata metadata(std::uint16_t boss_id = 321) {
    return domain::EncounterMetadata{
        .boss_id = boss_id,
        .pov_account = ":Broadcaster.1234",
    };
}

[[nodiscard]] domain::LogFileIdentity upload_file(std::string name) {
    return domain::LogFileIdentity{
        .canonical_path = std::filesystem::path{"logs"} / std::move(name),
        .size = 4096,
        .last_write_time = {},
    };
}

[[nodiscard]] domain::DpsReportResult dps_result() {
    return domain::DpsReportResult{
        .permalink = "https://dps.report/pump",
        .encounter_name = "Pump Boss",
        .boss_id = 321,
        .mode = "CM",
        .success = true,
    };
}

void creation_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};

    const auto no_results =
        ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                ApplicationPumpConfig{.max_metadata_results_per_tick = 0});
    MANNY_CHECK(suite, !no_results.has_value());
    MANNY_CHECK(suite, no_results.error().code == ApplicationPumpErrorCode::InvalidConfiguration);

    const auto no_upload_results =
        ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                ApplicationPumpConfig{.max_metadata_results_per_tick = 1,
                                                      .max_upload_results_per_tick = 0});
    MANNY_CHECK(suite, !no_upload_results.has_value());
    MANNY_CHECK(suite,
                no_upload_results.error().code == ApplicationPumpErrorCode::InvalidConfiguration);

    const auto bad_stability = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                       ApplicationPumpConfig{
                                                           .required_matching_observations = 1,
                                                           .dedupe_capacity = 8,
                                                           .max_metadata_results_per_tick = 1,
                                                       });
    MANNY_CHECK(suite, !bad_stability.has_value());
    MANNY_CHECK(suite, bad_stability.error().code == ApplicationPumpErrorCode::Discovery);
    MANNY_CHECK(suite, bad_stability.error().discovery_error.has_value());
}

void stable_submission_and_result_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                    .max_metadata_results_per_tick = 4,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(pump_created.value());
    MANNY_CHECK(suite, pump.max_metadata_results_per_tick() == 4);
    MANNY_CHECK(suite, pump.max_upload_results_per_tick() == 8);

    fixture.source.push(batch({observation("logs/encounter.zevtc")}));
    const auto first = pump.tick(dps_only());
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, first->observations == 1);
    MANNY_CHECK(suite, first->submitted_jobs == 0);
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 1);

    fixture.source.push(batch({observation("logs/encounter.zevtc")}));
    const auto stable = pump.tick(dps_only());
    MANNY_CHECK(suite, stable.has_value());
    MANNY_CHECK(suite, stable->submitted_jobs == 1);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
    MANNY_CHECK(suite, pump.dedupe_size() == 1);
    MANNY_CHECK(suite, fixture.dps.requests.empty());

    fixture.parser.results.push_back(ports::MetadataParseResult{
        .job_id = fixture.parser.requests.front().job_id,
        .metadata = metadata(),
    });
    fixture.source.push(batch({}, {}, true, false,
                              {ports::LogCandidateIssue{
                                  .path = "logs",
                                  .message = "transient scan warning",
                              }}));
    const auto parsed = pump.tick(dps_only());
    MANNY_CHECK(suite, parsed.has_value());
    MANNY_CHECK(suite, parsed->metadata_results_handled == 1);
    MANNY_CHECK(suite, !parsed->scan_complete);
    MANNY_CHECK(suite, parsed->source_issues.size() == 1);
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);
    MANNY_CHECK(suite, fixture.dps.requests.front().metadata.boss_id == 321);

    fixture.source.push(batch({observation("logs/encounter.zevtc")}));
    fixture.source.push(batch({observation("logs/encounter.zevtc")}));
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    const auto duplicate = pump.tick(dps_only());
    MANNY_CHECK(suite, duplicate.has_value());
    MANNY_CHECK(suite, duplicate->submitted_jobs == 0);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
}

void persisted_identity_seed_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(*upload_created);
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                    .max_metadata_results_per_tick = 4,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(*pump_created);

    const auto persisted_observation = observation("logs/persisted.zevtc");
    const std::array persisted{domain::LogFileIdentity{
        .canonical_path = persisted_observation.canonical_path,
        .size = persisted_observation.size,
        .last_write_time = persisted_observation.last_write_time,
    }};
    MANNY_CHECK(suite, pump.seed_processed_logs(persisted).has_value());
    MANNY_CHECK(suite, pump.dedupe_size() == 1);

    fixture.source.push(batch({persisted_observation}));
    fixture.source.push(batch({persisted_observation}));
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    const auto duplicate = pump.tick(dps_only());
    MANNY_CHECK(suite, duplicate.has_value());
    MANNY_CHECK(suite, duplicate->submitted_jobs == 0);
    MANNY_CHECK(suite, fixture.parser.requests.empty());

    fixture.source.push(batch({observation("logs/persisted.zevtc", 101)}));
    fixture.source.push(batch({observation("logs/persisted.zevtc", 101)}));
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    const auto changed = pump.tick(dps_only());
    MANNY_CHECK(suite, changed.has_value());
    MANNY_CHECK(suite, changed->submitted_jobs == 1);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
}

void upload_result_and_retry_pump_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(*upload_created);
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                    .max_metadata_results_per_tick = 1,
                                                    .max_upload_results_per_tick = 1,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(*pump_created);
    MANNY_CHECK(suite, pump.max_upload_results_per_tick() == 1);

    const auto job_id =
        upload.add_job(upload_file("provider-result.zevtc"), metadata(), dps_only());
    MANNY_CHECK(suite, job_id.has_value());
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);
    fixture.dps.results.push_back(ports::UploadResult{
        .job_id = *job_id,
        .provider = Provider::DpsReport,
        .outcome = ports::UploadOutcome::Retry,
        .detail = "retry",
        .retry_after = 10s,
        .dps_report_result = std::nullopt,
    });
    const auto retried = pump.tick(dps_only());
    MANNY_CHECK(suite, retried.has_value());
    MANNY_CHECK(suite, retried->upload_results_handled == 1);
    MANNY_CHECK(suite, retried->retries_dispatched == 0);

    fixture.clock.advance(10s);
    const auto dispatched = pump.tick(dps_only());
    MANNY_CHECK(suite, dispatched.has_value());
    MANNY_CHECK(suite, dispatched->upload_results_handled == 0);
    MANNY_CHECK(suite, dispatched->retries_dispatched == 1);
    MANNY_CHECK(suite, fixture.dps.requests.size() == 2);
    MANNY_CHECK(suite, fixture.dps.requests.back().attempt == 2);

    fixture.dps.results.push_back(ports::UploadResult{
        .job_id = *job_id,
        .provider = Provider::DpsReport,
        .outcome = ports::UploadOutcome::Succeeded,
        .detail = "uploaded",
        .retry_after = std::nullopt,
        .dps_report_result = dps_result(),
    });
    const auto succeeded = pump.tick(dps_only());
    MANNY_CHECK(suite, succeeded.has_value());
    MANNY_CHECK(suite, succeeded->upload_results_handled == 1);
    const auto snapshots = upload.snapshots();
    MANNY_CHECK(suite, snapshots.front().dps_report_result.has_value());
    MANNY_CHECK(suite, snapshots.front().dps_report_result->permalink == "https://dps.report/pump");
}

void removal_resets_stability_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion);
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(pump_created.value());

    fixture.source.push(batch({observation("logs/recreated.zevtc")}));
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 1);

    fixture.source.push(batch({}, {"logs/recreated.zevtc"}));
    const auto removed = pump.tick(dps_only());
    MANNY_CHECK(suite, removed.has_value());
    MANNY_CHECK(suite, removed->removed_paths == 1);
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 0);

    fixture.source.push(batch({observation("logs/recreated.zevtc")}));
    const auto after_recreation = pump.tick(dps_only());
    MANNY_CHECK(suite, after_recreation.has_value());
    MANNY_CHECK(suite, after_recreation->submitted_jobs == 0);
    fixture.source.push(batch({observation("logs/recreated.zevtc")}));
    const auto stable = pump.tick(dps_only());
    MANNY_CHECK(suite, stable.has_value());
    MANNY_CHECK(suite, stable->submitted_jobs == 1);
}

void live_stability_reconfiguration_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(*upload_created);
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(*pump_created);

    fixture.source.push(batch({observation("logs/reconfigured.zevtc")}));
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 1);

    const auto invalid = pump.update_required_matching_observations(1);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == ApplicationPumpErrorCode::Discovery);
    MANNY_CHECK(suite, pump.required_matching_observations() == 2);
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 1);

    MANNY_CHECK(suite, pump.update_required_matching_observations(3).has_value());
    MANNY_CHECK(suite, pump.required_matching_observations() == 3);
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 0);
    for (std::size_t index = 0; index < 3; ++index) {
        fixture.source.push(batch({observation("logs/reconfigured.zevtc")}));
    }
    const auto first = pump.tick(dps_only());
    const auto second = pump.tick(dps_only());
    const auto third = pump.tick(dps_only());
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, third.has_value());
    if (first && second && third) {
        MANNY_CHECK(suite, first->submitted_jobs == 0);
        MANNY_CHECK(suite, second->submitted_jobs == 0);
        MANNY_CHECK(suite, third->submitted_jobs == 1);
    }
    MANNY_CHECK(suite, pump.dedupe_size() == 1);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);

    fixture.source.push(batch({observation("logs/pending.zevtc")}));
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 1);
    pump.reset_pending_candidates();
    MANNY_CHECK(suite, pump.tracked_candidate_count() == 0);
    MANNY_CHECK(suite, pump.dedupe_size() == 1);

    for (std::size_t index = 0; index < 3; ++index) {
        fixture.source.push(batch({observation("logs/reconfigured.zevtc")}));
    }
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    const auto deduplicated = pump.tick(dps_only());
    MANNY_CHECK(suite, deduplicated.has_value());
    if (deduplicated) {
        MANNY_CHECK(suite, deduplicated->submitted_jobs == 0);
    }
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);

    pump.cancel_all();
    const auto stopped = pump.update_required_matching_observations(2);
    MANNY_CHECK(suite, !stopped.has_value());
    MANNY_CHECK(suite, stopped.error().code == ApplicationPumpErrorCode::ShuttingDown);
}

void bounded_result_drain_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                    .max_metadata_results_per_tick = 1,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(pump_created.value());

    const auto both = batch({observation("logs/a.zevtc"), observation("logs/b.zevtc")});
    fixture.source.push(both);
    fixture.source.push(both);
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    MANNY_CHECK(suite, fixture.parser.requests.size() == 2);

    fixture.parser.results.push_back(ports::MetadataParseResult{
        .job_id = fixture.parser.requests[0].job_id,
        .metadata = metadata(100),
    });
    fixture.parser.results.push_back(ports::MetadataParseResult{
        .job_id = fixture.parser.requests[1].job_id,
        .metadata = metadata(200),
    });
    const auto first = pump.tick(dps_only());
    MANNY_CHECK(suite, first.has_value());
    MANNY_CHECK(suite, first->metadata_results_handled == 1);
    MANNY_CHECK(suite, fixture.parser.results.size() == 1);
    MANNY_CHECK(suite, fixture.dps.requests.size() == 1);

    const auto second = pump.tick(dps_only());
    MANNY_CHECK(suite, second.has_value());
    MANNY_CHECK(suite, second->metadata_results_handled == 1);
    MANNY_CHECK(suite, fixture.parser.results.empty());
    MANNY_CHECK(suite, fixture.dps.requests.size() == 2);
}

void bounded_parser_dispatch_tests(TestSuite& suite) {
    PumpFixture fixture;
    fixture.parser.queue_capacity = 1;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(*upload_created);
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(*pump_created);

    const auto both = batch({observation("logs/a.zevtc"), observation("logs/b.zevtc")});
    fixture.source.push(both);
    fixture.source.push(both);
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    const auto saturated = pump.tick(dps_only());
    MANNY_CHECK(suite, saturated.has_value());
    MANNY_CHECK(suite, saturated->submitted_jobs == 1);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);
    MANNY_CHECK(suite, upload.snapshots().size() == 1);

    fixture.parser.results.push_back(ports::MetadataParseResult{
        .job_id = fixture.parser.requests.front().job_id,
        .metadata = metadata(100),
    });
    fixture.source.push(both);
    const auto resumed = pump.tick(dps_only());
    MANNY_CHECK(suite, resumed.has_value());
    MANNY_CHECK(suite, resumed->metadata_results_handled == 1);
    MANNY_CHECK(suite, resumed->submitted_jobs == 1);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 2);
    MANNY_CHECK(suite, fixture.parser.requests.back().file.canonical_path == "logs/b.zevtc");
    MANNY_CHECK(suite, upload.snapshots().size() == 2);
}

void failure_and_dedupe_rollback_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports, 1);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion,
                                                ApplicationPumpConfig{
                                                    .required_matching_observations = 2,
                                                    .dedupe_capacity = 8,
                                                    .max_metadata_results_per_tick = 2,
                                                });
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(pump_created.value());

    const auto both = batch({observation("logs/a.zevtc"), observation("logs/b.zevtc")});
    fixture.source.push(both);
    fixture.source.push(both);
    MANNY_CHECK(suite, pump.tick(dps_only()).has_value());
    const auto full = pump.tick(dps_only());
    MANNY_CHECK(suite, !full.has_value());
    MANNY_CHECK(suite, full.error().code == ApplicationPumpErrorCode::Ingestion);
    MANNY_CHECK(suite, full.error().coordinator_error ==
                           application::CoordinatorErrorCode::CapacityReached);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 1);

    fixture.parser.results.push_back(ports::MetadataParseResult{
        .job_id = fixture.parser.requests.front().job_id,
        .metadata = std::unexpected(ports::MetadataParseError{
            .code = ports::MetadataParseErrorCode::MalformedLog,
            .message = "settled first job",
        }),
    });
    fixture.source.push(batch({observation("logs/b.zevtc")}));
    const auto retry_first_observation = pump.tick(dps_only());
    MANNY_CHECK(suite, retry_first_observation.has_value());
    MANNY_CHECK(suite, retry_first_observation->metadata_results_handled == 1);
    MANNY_CHECK(suite, retry_first_observation->submitted_jobs == 0);

    fixture.source.push(batch({observation("logs/b.zevtc")}));
    const auto retried = pump.tick(dps_only());
    MANNY_CHECK(suite, retried.has_value());
    MANNY_CHECK(suite, retried->submitted_jobs == 1);
    MANNY_CHECK(suite, fixture.parser.requests.size() == 2);
    MANNY_CHECK(suite, fixture.parser.requests.back().file.canonical_path == "logs/b.zevtc");

    const auto polls_before_result_error = fixture.source.poll_count;
    fixture.parser.results.push_back(ports::MetadataParseResult{
        .job_id = domain::UploadJobId{999},
        .metadata = metadata(),
    });
    const auto unknown_result = pump.tick(dps_only());
    MANNY_CHECK(suite, !unknown_result.has_value());
    MANNY_CHECK(suite, unknown_result.error().code == ApplicationPumpErrorCode::Ingestion);
    MANNY_CHECK(suite, fixture.source.poll_count == polls_before_result_error);
}

void source_failure_and_shutdown_tests(TestSuite& suite) {
    PumpFixture fixture;
    auto upload_created = UploadCoordinator::create(fixture.clock, fixture.provider_ports);
    MANNY_CHECK(suite, upload_created.has_value());
    auto upload = std::move(upload_created.value());
    LogIngestionCoordinator ingestion{upload, fixture.parser};
    auto pump_created = ApplicationPump::create(fixture.source, fixture.parser, ingestion);
    MANNY_CHECK(suite, pump_created.has_value());
    auto pump = std::move(pump_created.value());

    fixture.source.fail(LogCandidateSourceErrorCode::ScanFailed, "directory unavailable");
    const auto failed = pump.tick(dps_only());
    MANNY_CHECK(suite, !failed.has_value());
    MANNY_CHECK(suite, failed.error().code == ApplicationPumpErrorCode::CandidateSource);
    MANNY_CHECK(suite,
                failed.error().candidate_source_error == LogCandidateSourceErrorCode::ScanFailed);

    pump.cancel_all();
    pump.cancel_all();
    MANNY_CHECK(suite, pump.is_shutting_down());
    MANNY_CHECK(suite, fixture.parser.cancel_count == 1);
    MANNY_CHECK(suite, fixture.dps.cancel_count == 1);
    MANNY_CHECK(suite, fixture.wingman.cancel_count == 1);
    MANNY_CHECK(suite, fixture.donbot.cancel_count == 1);
    MANNY_CHECK(suite, fixture.twitch.cancel_count == 1);

    const auto polls_before_shutdown_tick = fixture.source.poll_count;
    const auto stopped = pump.tick(dps_only());
    MANNY_CHECK(suite, !stopped.has_value());
    MANNY_CHECK(suite, stopped.error().code == ApplicationPumpErrorCode::ShuttingDown);
    MANNY_CHECK(suite, fixture.source.poll_count == polls_before_shutdown_tick);
}

} // namespace

void run_application_pump_tests(TestSuite& suite) {
    creation_tests(suite);
    stable_submission_and_result_tests(suite);
    persisted_identity_seed_tests(suite);
    removal_resets_stability_tests(suite);
    live_stability_reconfiguration_tests(suite);
    bounded_result_drain_tests(suite);
    bounded_parser_dispatch_tests(suite);
    upload_result_and_retry_pump_tests(suite);
    failure_and_dedupe_rollback_tests(suite);
    source_failure_and_shutdown_tests(suite);
}

} // namespace manny_uploader::test
