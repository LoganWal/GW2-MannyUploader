#include "manny_uploader/domain/upload_job.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace manny_uploader::test {
namespace {

using domain::DpsReportResult;
using domain::JobErrorCode;
using domain::LogFileIdentity;
using domain::Provider;
using domain::ProviderSelection;
using domain::ProviderState;
using domain::UploadJob;
using domain::UploadJobId;

[[nodiscard]] LogFileIdentity example_file() {
    return LogFileIdentity{
        .canonical_path = std::filesystem::path{"logs/example.zevtc"},
        .size = 1234,
        .last_write_time = {},
    };
}

[[nodiscard]] ProviderSelection providers(bool dps, bool wingman, bool donbot, bool twitch) {
    ProviderSelection selection{};
    selection[domain::provider_index(Provider::DpsReport)] = dps;
    selection[domain::provider_index(Provider::Wingman)] = wingman;
    selection[domain::provider_index(Provider::DonBot)] = donbot;
    selection[domain::provider_index(Provider::Twitch)] = twitch;
    return selection;
}

void creation_tests(TestSuite& suite) {
    const auto invalid_id = UploadJob::create(
        UploadJobId{}, example_file(), UploadJob::DetectedAt{}, providers(true, true, true, true));
    MANNY_CHECK(suite, !invalid_id.has_value());
    MANNY_CHECK(suite, invalid_id.error().code == JobErrorCode::InvalidJobId);

    auto empty_file = example_file();
    empty_file.canonical_path.clear();
    const auto empty_path =
        UploadJob::create(UploadJobId{1}, std::move(empty_file), UploadJob::DetectedAt{},
                          providers(true, true, true, true));
    MANNY_CHECK(suite, !empty_path.has_value());
    MANNY_CHECK(suite, empty_path.error().code == JobErrorCode::EmptyPath);

    const auto twitch_without_dps =
        UploadJob::create(UploadJobId{1}, example_file(), UploadJob::DetectedAt{},
                          providers(false, false, false, true));
    MANNY_CHECK(suite, !twitch_without_dps.has_value());
    MANNY_CHECK(suite, twitch_without_dps.error().code == JobErrorCode::TwitchRequiresDpsReport);

    auto created = UploadJob::create(UploadJobId{42}, example_file(), UploadJob::DetectedAt{},
                                     providers(true, false, true, true));
    MANNY_CHECK(suite, created.has_value());
    MANNY_CHECK(suite, created->id() == UploadJobId{42});
    MANNY_CHECK(suite,
                created->provider_status(Provider::DpsReport).state == ProviderState::Waiting);
    MANNY_CHECK(suite,
                created->provider_status(Provider::Wingman).state == ProviderState::Disabled);
    MANNY_CHECK(suite, created->provider_status(Provider::DonBot).state == ProviderState::Waiting);
    MANNY_CHECK(suite, created->provider_status(Provider::Twitch).state == ProviderState::Waiting);
}

void transition_tests(TestSuite& suite) {
    auto created = UploadJob::create(UploadJobId{7}, example_file(), UploadJob::DetectedAt{},
                                     providers(true, true, false, true));
    MANNY_CHECK(suite, created.has_value());
    auto job = std::move(created.value());

    const auto wingman_active = job.transition(Provider::Wingman, ProviderState::Active);
    MANNY_CHECK(suite, wingman_active.has_value());
    MANNY_CHECK(suite, job.provider_status(Provider::Wingman).attempts == 1);

    const auto wingman_succeeded =
        job.transition(Provider::Wingman, ProviderState::Succeeded, "accepted");
    MANNY_CHECK(suite, wingman_succeeded.has_value());
    MANNY_CHECK(suite, domain::is_terminal(job.provider_status(Provider::Wingman).state));

    const auto terminal_transition = job.transition(Provider::Wingman, ProviderState::Waiting);
    MANNY_CHECK(suite, !terminal_transition.has_value());
    MANNY_CHECK(suite, terminal_transition.error().code == JobErrorCode::InvalidTransition);

    const auto dps_active = job.transition(Provider::DpsReport, ProviderState::Active);
    MANNY_CHECK(suite, dps_active.has_value());

    const auto retry_without_time =
        job.transition(Provider::DpsReport, ProviderState::RetryScheduled);
    MANNY_CHECK(suite, !retry_without_time.has_value());
    MANNY_CHECK(suite, retry_without_time.error().code == JobErrorCode::RetryTimeRequired);

    const auto retry_at = UploadJob::RetryAt{} + std::chrono::seconds{30};
    const auto retry = job.transition(Provider::DpsReport, ProviderState::RetryScheduled,
                                      "rate limited", retry_at);
    MANNY_CHECK(suite, retry.has_value());
    MANNY_CHECK(suite,
                job.provider_status(Provider::DpsReport).state == ProviderState::RetryScheduled);
    MANNY_CHECK(suite, job.provider_status(Provider::DpsReport).retry_at == retry_at);

    const auto second_attempt = job.transition(Provider::DpsReport, ProviderState::Active);
    MANNY_CHECK(suite, second_attempt.has_value());
    MANNY_CHECK(suite, job.provider_status(Provider::DpsReport).attempts == 2);

    const auto generic_dps_success = job.transition(Provider::DpsReport, ProviderState::Succeeded);
    MANNY_CHECK(suite, !generic_dps_success.has_value());
    MANNY_CHECK(suite, generic_dps_success.error().code == JobErrorCode::DpsReportResultRequired);
}

void dps_report_and_twitch_tests(TestSuite& suite) {
    auto created = UploadJob::create(UploadJobId{9}, example_file(), UploadJob::DetectedAt{},
                                     providers(true, false, false, true));
    MANNY_CHECK(suite, created.has_value());
    auto job = std::move(created.value());

    const auto early_twitch = job.transition(Provider::Twitch, ProviderState::Active);
    MANNY_CHECK(suite, !early_twitch.has_value());
    MANNY_CHECK(suite, early_twitch.error().code == JobErrorCode::DpsReportResultRequired);

    MANNY_CHECK(suite, job.transition(Provider::DpsReport, ProviderState::Active).has_value());

    const auto empty_permalink = job.complete_dps_report(DpsReportResult{});
    MANNY_CHECK(suite, !empty_permalink.has_value());
    MANNY_CHECK(suite, empty_permalink.error().code == JobErrorCode::EmptyPermalink);

    auto dps_result = DpsReportResult{
        .permalink = "https://dps.report/example",
        .encounter_name = "Example Encounter",
        .mode = "CM",
        .success = true,
    };
    const auto dps_completed = job.complete_dps_report(std::move(dps_result), "uploaded");
    MANNY_CHECK(suite, dps_completed.has_value());
    MANNY_CHECK(suite, job.provider_status(Provider::DpsReport).state == ProviderState::Succeeded);
    MANNY_CHECK(suite, job.dps_report_result().has_value());
    MANNY_CHECK(suite,
                job.dps_report_result()->permalink == std::string{"https://dps.report/example"});

    const auto twitch_active = job.transition(Provider::Twitch, ProviderState::Active);
    MANNY_CHECK(suite, twitch_active.has_value());
    const auto missing_receipt =
        job.transition(Provider::Twitch, ProviderState::Succeeded, "message sent");
    MANNY_CHECK(suite, !missing_receipt.has_value());
    MANNY_CHECK(suite, missing_receipt.error().code == JobErrorCode::InvalidTwitchDelivery);

    const auto missing_id = job.record_twitch_delivery(domain::TwitchDeliveryReceipt{
        .status = domain::TwitchDeliveryStatus::Sent,
        .message_id = std::nullopt,
    });
    MANNY_CHECK(suite, !missing_id.has_value());
    MANNY_CHECK(suite, missing_id.error().code == JobErrorCode::InvalidTwitchDelivery);
    const auto drop_with_id = job.record_twitch_delivery(domain::TwitchDeliveryReceipt{
        .status = domain::TwitchDeliveryStatus::AutoMod,
        .message_id = "unexpected",
    });
    MANNY_CHECK(suite, !drop_with_id.has_value());
    const auto receipt = job.record_twitch_delivery(domain::TwitchDeliveryReceipt{
        .status = domain::TwitchDeliveryStatus::Sent,
        .message_id = "message-123",
    });
    MANNY_CHECK(suite, receipt.has_value());
    MANNY_CHECK(suite, job.twitch_delivery_receipt().has_value());
    const auto duplicate_receipt = job.record_twitch_delivery(domain::TwitchDeliveryReceipt{
        .status = domain::TwitchDeliveryStatus::Sent,
        .message_id = "message-456",
    });
    MANNY_CHECK(suite, !duplicate_receipt.has_value());
    MANNY_CHECK(suite,
                duplicate_receipt.error().code == JobErrorCode::TwitchDeliveryAlreadyRecorded);
    const auto twitch_success =
        job.transition(Provider::Twitch, ProviderState::Succeeded, "message sent");
    MANNY_CHECK(suite, twitch_success.has_value());
}

void metadata_tests(TestSuite& suite) {
    auto created = UploadJob::create(UploadJobId{11}, example_file(), UploadJob::DetectedAt{},
                                     providers(true, false, false, false));
    MANNY_CHECK(suite, created.has_value());
    auto job = std::move(created.value());

    auto metadata = domain::EncounterMetadata{.boss_id = 123, .pov_account = "Player.1234"};
    MANNY_CHECK(suite, job.set_encounter_metadata(std::move(metadata)).has_value());
    MANNY_CHECK(suite, job.encounter_metadata().has_value());
    MANNY_CHECK(suite, job.encounter_metadata()->boss_id == 123);

    const auto duplicate = job.set_encounter_metadata(
        domain::EncounterMetadata{.boss_id = 456, .pov_account = "Other.5678"});
    MANNY_CHECK(suite, !duplicate.has_value());
    MANNY_CHECK(suite, duplicate.error().code == JobErrorCode::MetadataAlreadySet);
}

void wingman_receipt_tests(TestSuite& suite) {
    auto created = UploadJob::create(UploadJobId{13}, example_file(), UploadJob::DetectedAt{},
                                     providers(false, true, false, false));
    MANNY_CHECK(suite, created.has_value());
    auto job = std::move(*created);

    const auto inactive = job.record_wingman_upload(
        domain::WingmanUploadReceipt{.permalink = "https://example.invalid/inactive"});
    MANNY_CHECK(suite, !inactive.has_value());
    MANNY_CHECK(suite, inactive.error().code == JobErrorCode::InvalidTransition);
    MANNY_CHECK(suite, job.transition(Provider::Wingman, ProviderState::Active).has_value());
    const auto empty = job.record_wingman_upload(domain::WingmanUploadReceipt{});
    MANNY_CHECK(suite, !empty.has_value());
    MANNY_CHECK(suite, empty.error().code == JobErrorCode::InvalidWingmanUpload);
    MANNY_CHECK(suite, job
                           .record_wingman_upload(domain::WingmanUploadReceipt{
                               .permalink = "https://gw2wingman.nevermindcreations.de/log/example",
                           })
                           .has_value());
    MANNY_CHECK(suite, job.wingman_upload_receipt().has_value());
    const auto duplicate = job.record_wingman_upload(domain::WingmanUploadReceipt{
        .permalink = "https://gw2wingman.nevermindcreations.de/log/duplicate",
    });
    MANNY_CHECK(suite, !duplicate.has_value());
    MANNY_CHECK(suite, duplicate.error().code == JobErrorCode::WingmanUploadAlreadyRecorded);
}

void donbot_receipt_tests(TestSuite& suite) {
    auto created = UploadJob::create(UploadJobId{14}, example_file(), UploadJob::DetectedAt{},
                                     providers(false, false, true, false));
    MANNY_CHECK(suite, created.has_value());
    auto job = std::move(*created);
    MANNY_CHECK(suite, job.transition(Provider::DonBot, ProviderState::Active).has_value());

    auto invalid_not_requested = job.record_donbot_upload(domain::DonBotUploadReceipt{
        .upload_id = 42,
        .fight_log_id = 314,
        .discord_delivery =
            domain::DonBotDiscordDeliveryReceipt{
                .outcome = domain::DonBotDiscordDeliveryOutcome::NotRequested,
                .sent = 1,
            },
    });
    MANNY_CHECK(suite, !invalid_not_requested.has_value());
    MANNY_CHECK(suite, invalid_not_requested.error().code == JobErrorCode::InvalidDonBotUpload);

    auto invalid_partial = job.record_donbot_upload(domain::DonBotUploadReceipt{
        .upload_id = 42,
        .fight_log_id = 314,
        .discord_delivery =
            domain::DonBotDiscordDeliveryReceipt{
                .outcome = domain::DonBotDiscordDeliveryOutcome::Partial,
                .sent = 2,
            },
    });
    MANNY_CHECK(suite, !invalid_partial.has_value());

    MANNY_CHECK(suite, job
                           .record_donbot_upload(domain::DonBotUploadReceipt{
                               .upload_id = 42,
                               .fight_log_id = 314,
                               .discord_delivery =
                                   domain::DonBotDiscordDeliveryReceipt{
                                       .outcome = domain::DonBotDiscordDeliveryOutcome::Partial,
                                       .sent = 2,
                                       .skipped = 1,
                                   },
                           })
                           .has_value());
    MANNY_CHECK(suite, job.donbot_upload_receipt().has_value());
    MANNY_CHECK(suite, job.donbot_upload_receipt()->discord_delivery.sent == 2);
}

void manual_retry_tests(TestSuite& suite) {
    auto created = UploadJob::create(UploadJobId{12}, example_file(), UploadJob::DetectedAt{},
                                     providers(true, true, true, true));
    MANNY_CHECK(suite, created.has_value());
    auto job = std::move(*created);

    const auto too_early = job.prepare_manual_retry(Provider::DpsReport);
    MANNY_CHECK(suite, !too_early.has_value());
    MANNY_CHECK(suite, too_early.error().code == JobErrorCode::ManualRetryRequiresFailure);

    MANNY_CHECK(suite, job.transition(Provider::DpsReport, ProviderState::Active).has_value());
    MANNY_CHECK(suite,
                job.transition(Provider::DpsReport, ProviderState::Failed, "failed").has_value());
    for (const auto dependent : {Provider::Wingman, Provider::DonBot, Provider::Twitch}) {
        MANNY_CHECK(suite, job.transition(dependent, ProviderState::Skipped,
                                          "Skipped because dps.report failed")
                               .has_value());
    }
    MANNY_CHECK(suite, job.prepare_manual_retry(Provider::DpsReport).has_value());
    MANNY_CHECK(suite, job.provider_status(Provider::DpsReport).state == ProviderState::Waiting);
    for (const auto dependent : {Provider::Wingman, Provider::DonBot, Provider::Twitch}) {
        MANNY_CHECK(suite, job.provider_status(dependent).state == ProviderState::Waiting);
    }

    MANNY_CHECK(suite, job.transition(Provider::DpsReport, ProviderState::Active).has_value());
    MANNY_CHECK(suite, job.complete_dps_report(DpsReportResult{
                                                   .permalink = "https://dps.report/retry",
                                                   .encounter_name = "Retry",
                                                   .boss_id = 1,
                                                   .mode = "NM",
                                                   .success = true,
                                               })
                           .has_value());
    MANNY_CHECK(suite, job.transition(Provider::Twitch, ProviderState::Active).has_value());
    MANNY_CHECK(suite,
                job.record_twitch_delivery(domain::TwitchDeliveryReceipt{
                                               .status = domain::TwitchDeliveryStatus::AutoMod,
                                               .message_id = std::nullopt,
                                           })
                    .has_value());
    MANNY_CHECK(suite, job.transition(Provider::Twitch, ProviderState::Failed, "held").has_value());
    MANNY_CHECK(suite, job.prepare_manual_retry(Provider::Twitch).has_value());
    MANNY_CHECK(suite, !job.twitch_delivery_receipt().has_value());
    MANNY_CHECK(suite, job.provider_status(Provider::Twitch).state == ProviderState::Waiting);
}

} // namespace

void run_upload_job_tests(TestSuite& suite) {
    creation_tests(suite);
    transition_tests(suite);
    dps_report_and_twitch_tests(suite);
    metadata_tests(suite);
    wingman_receipt_tests(suite);
    donbot_receipt_tests(suite);
    manual_retry_tests(suite);

    MANNY_CHECK(suite, domain::provider_name(Provider::DpsReport) == "dps.report");
    MANNY_CHECK(suite, domain::provider_name(Provider::Wingman) == "GW2Wingman");
    MANNY_CHECK(suite, domain::provider_name(Provider::DonBot) == "DonBot");
    MANNY_CHECK(suite, domain::provider_name(Provider::Twitch) == "Twitch");
}

} // namespace manny_uploader::test
