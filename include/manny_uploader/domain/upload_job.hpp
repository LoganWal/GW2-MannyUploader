#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace manny_uploader::domain {

enum class Provider : std::uint8_t {
    DpsReport,
    Wingman,
    DonBot,
    Twitch,
};

inline constexpr std::size_t provider_count = 4;
using ProviderSelection = std::array<bool, provider_count>;

enum class ProviderState : std::uint8_t {
    Disabled,
    Waiting,
    Active,
    Succeeded,
    Skipped,
    RetryScheduled,
    Failed,
    Cancelled,
};

struct UploadJobId {
    std::uint64_t value{};

    [[nodiscard]] friend constexpr bool operator==(UploadJobId, UploadJobId) noexcept = default;
};

struct LogFileIdentity {
    std::filesystem::path canonical_path;
    std::uintmax_t size{};
    std::filesystem::file_time_type last_write_time{};

    [[nodiscard]] friend bool operator==(const LogFileIdentity&, const LogFileIdentity&) = default;
};

struct EncounterMetadata {
    std::uint16_t boss_id{};
    std::string pov_account;
    std::optional<std::uint16_t> remaining_health_basis_points{};

    [[nodiscard]] friend bool operator==(const EncounterMetadata&,
                                         const EncounterMetadata&) = default;
};

struct WingmanUploadReceipt {
    std::string permalink;

    [[nodiscard]] friend bool operator==(const WingmanUploadReceipt&,
                                         const WingmanUploadReceipt&) = default;
};

struct DpsReportResult {
    std::string permalink;
    std::string encounter_name;
    std::uint16_t boss_id{};
    std::string mode;
    bool success{};

    [[nodiscard]] friend bool operator==(const DpsReportResult&, const DpsReportResult&) = default;
};

enum class DonBotDiscordDeliveryMode : std::uint8_t {
    None,
    GuildDefaults,
    ChannelOverride,
};

enum class DonBotDiscordDeliveryOutcome : std::uint8_t {
    NotRequested,
    Sent,
    Partial,
    Skipped,
    Failed,
    Ambiguous,
};

struct DonBotDiscordDeliveryReceipt {
    DonBotDiscordDeliveryOutcome outcome{DonBotDiscordDeliveryOutcome::NotRequested};
    std::uint16_t sent{};
    std::uint16_t skipped{};
    std::uint16_t failed{};
    std::uint16_t ambiguous{};

    [[nodiscard]] friend bool operator==(const DonBotDiscordDeliveryReceipt&,
                                         const DonBotDiscordDeliveryReceipt&) = default;
};

struct DonBotUploadReceipt {
    std::optional<std::uint64_t> upload_id;
    std::optional<std::uint64_t> fight_log_id;
    DonBotDiscordDeliveryReceipt discord_delivery;

    [[nodiscard]] friend bool operator==(const DonBotUploadReceipt&,
                                         const DonBotUploadReceipt&) = default;
};

enum class TwitchDeliveryStatus : std::uint8_t {
    Sent,
    AutoMod,
    BlockedTerm,
    Duplicate,
    RateLimited,
    FollowersOnly,
    SlowMode,
    SubscribersOnly,
    Restricted,
    OtherDrop,
};

struct TwitchDeliveryReceipt {
    TwitchDeliveryStatus status;
    std::optional<std::string> message_id;

    [[nodiscard]] friend bool operator==(const TwitchDeliveryReceipt&,
                                         const TwitchDeliveryReceipt&) = default;
};

struct ProviderStatus {
    ProviderState state{ProviderState::Disabled};
    std::uint32_t attempts{};
    std::string detail;
    std::optional<std::chrono::steady_clock::time_point> retry_at;

    [[nodiscard]] friend bool operator==(const ProviderStatus&, const ProviderStatus&) = default;
};

enum class JobErrorCode : std::uint8_t {
    InvalidJobId,
    EmptyPath,
    TwitchRequiresDpsReport,
    UnknownProvider,
    InvalidTransition,
    RetryTimeRequired,
    UnexpectedRetryTime,
    DpsReportResultRequired,
    EmptyPermalink,
    MetadataAlreadySet,
    InvalidTwitchDelivery,
    TwitchDeliveryAlreadyRecorded,
    InvalidDonBotUpload,
    DonBotUploadAlreadyRecorded,
    InvalidWingmanUpload,
    WingmanUploadAlreadyRecorded,
    ManualRetryRequiresFailure,
    ExplicitDeliveryBusy,
    EncounterMetadataRequired,
};

struct JobError {
    JobErrorCode code;
    std::string message;
};

struct UploadJobRecord {
    LogFileIdentity file;
    std::chrono::system_clock::time_point detected_at;
    std::optional<EncounterMetadata> encounter_metadata;
    std::optional<DpsReportResult> dps_report_result;
    std::optional<WingmanUploadReceipt> wingman_upload_receipt{};
    std::optional<DonBotUploadReceipt> donbot_upload_receipt;
    std::optional<TwitchDeliveryReceipt> twitch_delivery_receipt;
    std::array<ProviderStatus, provider_count> providers;

    [[nodiscard]] friend bool operator==(const UploadJobRecord&,
                                         const UploadJobRecord&) noexcept = default;
};

class UploadJob {
  public:
    using DetectedAt = std::chrono::system_clock::time_point;
    using RetryAt = std::chrono::steady_clock::time_point;

    [[nodiscard]] static std::expected<UploadJob, JobError>
    create(UploadJobId id, LogFileIdentity file, DetectedAt detected_at,
           const ProviderSelection& enabled_providers);
    [[nodiscard]] static std::expected<UploadJob, JobError> restore(UploadJobId id,
                                                                    UploadJobRecord record);

    [[nodiscard]] UploadJobId id() const noexcept;
    [[nodiscard]] const LogFileIdentity& file() const noexcept;
    [[nodiscard]] DetectedAt detected_at() const noexcept;
    [[nodiscard]] const ProviderStatus& provider_status(Provider provider) const noexcept;
    [[nodiscard]] const std::optional<EncounterMetadata>& encounter_metadata() const noexcept;
    [[nodiscard]] const std::optional<DpsReportResult>& dps_report_result() const noexcept;
    [[nodiscard]] const std::optional<WingmanUploadReceipt>&
    wingman_upload_receipt() const noexcept;
    [[nodiscard]] const std::optional<DonBotUploadReceipt>& donbot_upload_receipt() const noexcept;
    [[nodiscard]] const std::optional<TwitchDeliveryReceipt>&
    twitch_delivery_receipt() const noexcept;

    [[nodiscard]] std::expected<void, JobError> set_encounter_metadata(EncounterMetadata metadata);

    [[nodiscard]] std::expected<void, JobError>
    transition(Provider provider, ProviderState next, std::string detail = {},
               std::optional<RetryAt> retry_at = std::nullopt);

    [[nodiscard]] std::expected<void, JobError> complete_dps_report(DpsReportResult result,
                                                                    std::string detail = {});
    [[nodiscard]] std::expected<void, JobError>
    record_twitch_delivery(TwitchDeliveryReceipt receipt);
    [[nodiscard]] std::expected<void, JobError> record_wingman_upload(WingmanUploadReceipt receipt);
    [[nodiscard]] std::expected<void, JobError> record_donbot_upload(DonBotUploadReceipt receipt);
    [[nodiscard]] std::expected<void, JobError> prepare_manual_retry(Provider provider);
    [[nodiscard]] std::expected<void, JobError> prepare_explicit_delivery(Provider provider);
    [[nodiscard]] UploadJobRecord record() const;

  private:
    UploadJob(UploadJobId id, LogFileIdentity file, DetectedAt detected_at,
              const ProviderSelection& enabled_providers);

    UploadJobId id_;
    LogFileIdentity file_;
    DetectedAt detected_at_;
    std::array<ProviderStatus, provider_count> providers_;
    std::optional<EncounterMetadata> encounter_metadata_;
    std::optional<DpsReportResult> dps_report_result_;
    std::optional<WingmanUploadReceipt> wingman_upload_receipt_;
    std::optional<DonBotUploadReceipt> donbot_upload_receipt_;
    std::optional<TwitchDeliveryReceipt> twitch_delivery_receipt_;
};

[[nodiscard]] constexpr std::size_t provider_index(Provider provider) noexcept {
    return static_cast<std::size_t>(provider);
}

[[nodiscard]] std::string_view provider_name(Provider provider) noexcept;
[[nodiscard]] bool is_terminal(ProviderState state) noexcept;

} // namespace manny_uploader::domain
