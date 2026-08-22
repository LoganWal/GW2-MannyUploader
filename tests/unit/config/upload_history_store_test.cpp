#include "manny_uploader/config/upload_history_store.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

class TempHistoryTree {
  public:
    TempHistoryTree() {
        static std::uint64_t sequence{};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("manny-history-" + std::to_string(timestamp) + "-" + std::to_string(++sequence));
        std::error_code error;
        if (!std::filesystem::create_directories(root_, error) || error) {
            throw std::runtime_error{"Could not create upload history test directory"};
        }
    }

    ~TempHistoryTree() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove_all(root_, error));
    }

    TempHistoryTree(const TempHistoryTree&) = delete;
    TempHistoryTree& operator=(const TempHistoryTree&) = delete;

    [[nodiscard]] std::filesystem::path path() const {
        return root_ / "nested" / "upload-history.json";
    }

    void write(std::string_view contents) const {
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(path().parent_path(), error));
        if (error) {
            throw std::runtime_error{"Could not create upload history fixture directory"};
        }
        std::ofstream stream{path(), std::ios::binary | std::ios::trunc};
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Could not write upload history fixture"};
        }
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] domain::UploadJobRecord record(std::uint64_t identity,
                                             domain::ProviderState dps_state) {
    std::array<domain::ProviderStatus, domain::provider_count> providers{};
    providers[domain::provider_index(domain::Provider::DpsReport)] = domain::ProviderStatus{
        .state = dps_state,
        .attempts = 2,
        .detail = "dps detail",
        .retry_at = std::nullopt,
    };
    providers[domain::provider_index(domain::Provider::Wingman)] = domain::ProviderStatus{
        .state = domain::ProviderState::Disabled,
        .attempts = 0,
        .detail = {},
        .retry_at = std::nullopt,
    };
    providers[domain::provider_index(domain::Provider::DonBot)] = domain::ProviderStatus{
        .state = domain::ProviderState::Succeeded,
        .attempts = 1,
        .detail = "Uploaded to DonBot",
        .retry_at = std::nullopt,
    };
    providers[domain::provider_index(domain::Provider::Twitch)] = domain::ProviderStatus{
        .state = domain::ProviderState::Succeeded,
        .attempts = 1,
        .detail = "Posted to Twitch chat",
        .retry_at = std::nullopt,
    };
    return domain::UploadJobRecord{
        .file =
            domain::LogFileIdentity{
                .canonical_path =
                    std::filesystem::path{"logs"} / (std::to_string(identity) + ".zevtc"),
                .size = 4096 + identity,
                .last_write_time =
                    std::filesystem::file_time_type{std::filesystem::file_time_type::duration{
                        static_cast<std::int64_t>(identity)}},
            },
        .detected_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{
            static_cast<std::int64_t>(identity * 1000)}},
        .encounter_metadata =
            domain::EncounterMetadata{
                .boss_id = 123,
                .pov_account = "Broadcaster.1234",
            },
        .dps_report_result =
            domain::DpsReportResult{
                .permalink = "https://dps.report/example-" + std::to_string(identity),
                .encounter_name = "Example Encounter",
                .boss_id = 123,
                .mode = "CM",
                .success = true,
            },
        .donbot_upload_receipt =
            domain::DonBotUploadReceipt{
                .upload_id = identity + 100,
                .fight_log_id = identity + 200,
            },
        .twitch_delivery_receipt =
            domain::TwitchDeliveryReceipt{
                .status = domain::TwitchDeliveryStatus::Sent,
                .message_id = "message-" + std::to_string(identity),
            },
        .providers = std::move(providers),
    };
}

void round_trip_and_merge_tests(TestSuite& suite) {
    TempHistoryTree tree;
    auto created = config::UploadHistoryStore::create(tree.path(), 2);
    MANNY_CHECK(suite, created.has_value());
    auto store = std::move(*created);
    MANNY_CHECK(suite, store.records().empty());
    MANNY_CHECK(suite, store.recovery_diagnostic().empty());

    auto first = record(1, domain::ProviderState::Succeeded);
    first.file.canonical_path = std::filesystem::path{u8"logs/ström-1.zevtc"};
    MANNY_CHECK(suite, store.merge_and_save(std::array{first}).has_value());

    auto reopened = config::UploadHistoryStore::create(tree.path(), 2);
    MANNY_CHECK(suite, reopened.has_value());
    MANNY_CHECK(suite, reopened->records() == std::vector{first});

    auto updated_first = first;
    updated_first.providers[domain::provider_index(domain::Provider::DpsReport)].detail =
        "explicitly uploaded again";
    const auto second = record(2, domain::ProviderState::Succeeded);
    MANNY_CHECK(suite, reopened->merge_and_save(std::array{updated_first, second}).has_value());
    MANNY_CHECK(suite, reopened->records().size() == 2);
    MANNY_CHECK(suite, reopened->records().front() == updated_first);

    const auto third = record(3, domain::ProviderState::Succeeded);
    MANNY_CHECK(suite, reopened->merge_and_save(std::array{third}).has_value());
    MANNY_CHECK(suite, reopened->records().size() == 2);
    MANNY_CHECK(suite, reopened->records().front() == second);
    MANNY_CHECK(suite, reopened->records().back() == third);
}

void recovery_and_validation_tests(TestSuite& suite) {
    TempHistoryTree tree;
    tree.write("{not valid json");
    auto recovered = config::UploadHistoryStore::create(tree.path());
    MANNY_CHECK(suite, recovered.has_value());
    MANNY_CHECK(suite, recovered->records().empty());
    MANNY_CHECK(suite, !recovered->recovery_diagnostic().empty());

    const auto invalid_path = config::UploadHistoryStore::create({});
    MANNY_CHECK(suite, !invalid_path.has_value());
    MANNY_CHECK(suite, invalid_path.error().code ==
                           config::UploadHistoryStoreErrorCode::InvalidConfiguration);
    const auto invalid_capacity = config::UploadHistoryStore::create(tree.path(), 0);
    MANNY_CHECK(suite, !invalid_capacity.has_value());
    MANNY_CHECK(suite, invalid_capacity.error().code ==
                           config::UploadHistoryStoreErrorCode::InvalidConfiguration);
}

} // namespace

void run_upload_history_store_tests(TestSuite& suite) {
    round_trip_and_merge_tests(suite);
    recovery_and_validation_tests(suite);
}

} // namespace manny_uploader::test
