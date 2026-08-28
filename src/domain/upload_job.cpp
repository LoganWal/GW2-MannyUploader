#include "manny_uploader/domain/upload_job.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace manny_uploader::domain {
namespace {

[[nodiscard]] bool is_known_provider(Provider provider) noexcept {
    return provider_index(provider) < provider_count;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool is_allowed_transition(ProviderState current, ProviderState next) noexcept {
    switch (current) {
    case ProviderState::Waiting:
        return next == ProviderState::Active || next == ProviderState::Skipped ||
               next == ProviderState::Failed || next == ProviderState::Cancelled;
    case ProviderState::Active:
        return next == ProviderState::Succeeded || next == ProviderState::RetryScheduled ||
               next == ProviderState::Failed || next == ProviderState::Cancelled;
    case ProviderState::RetryScheduled:
        return next == ProviderState::Active || next == ProviderState::Failed ||
               next == ProviderState::Cancelled;
    case ProviderState::Failed:
        return next == ProviderState::Waiting || next == ProviderState::Cancelled;
    case ProviderState::Disabled:
    case ProviderState::Succeeded:
    case ProviderState::Skipped:
    case ProviderState::Cancelled:
        return false;
    }

    return false;
}

[[nodiscard]] JobError make_error(JobErrorCode code, std::string message) {
    return JobError{.code = code, .message = std::move(message)};
}

[[nodiscard]] bool valid_twitch_delivery_status(TwitchDeliveryStatus status) noexcept {
    return status >= TwitchDeliveryStatus::Sent && status <= TwitchDeliveryStatus::OtherDrop;
}

[[nodiscard]] bool valid_message_id(const std::optional<std::string>& message_id) noexcept {
    return message_id && !message_id->empty() && message_id->size() <= 256 &&
           std::ranges::all_of(*message_id, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte >= 0x21U && byte <= 0x7eU;
           });
}

[[nodiscard]] bool
valid_donbot_discord_delivery(const DonBotDiscordDeliveryReceipt& receipt) noexcept {
    constexpr std::uint16_t maximum_messages = 4;
    const auto total = static_cast<std::uint32_t>(receipt.sent) + receipt.skipped + receipt.failed +
                       receipt.ambiguous;
    if (total > maximum_messages) {
        return false;
    }
    if (receipt.outcome == DonBotDiscordDeliveryOutcome::NotRequested) {
        return total == 0;
    }
    const auto populated_categories =
        static_cast<unsigned>(receipt.sent != 0) + static_cast<unsigned>(receipt.skipped != 0) +
        static_cast<unsigned>(receipt.failed != 0) + static_cast<unsigned>(receipt.ambiguous != 0);
    switch (receipt.outcome) {
    case DonBotDiscordDeliveryOutcome::NotRequested:
        return false;
    case DonBotDiscordDeliveryOutcome::Sent:
        return receipt.sent != 0 && populated_categories == 1;
    case DonBotDiscordDeliveryOutcome::Partial:
        return populated_categories >= 2;
    case DonBotDiscordDeliveryOutcome::Skipped:
        return receipt.skipped != 0 && populated_categories == 1;
    case DonBotDiscordDeliveryOutcome::Failed:
        return receipt.failed != 0 && populated_categories == 1;
    case DonBotDiscordDeliveryOutcome::Ambiguous:
        return receipt.ambiguous != 0 && populated_categories == 1;
    }
    return false;
}

[[nodiscard]] bool valid_donbot_upload(const DonBotUploadReceipt& receipt) noexcept {
    return (!receipt.upload_id || *receipt.upload_id != 0) &&
           (!receipt.fight_log_id || *receipt.fight_log_id != 0) &&
           valid_donbot_discord_delivery(receipt.discord_delivery);
}

} // namespace

std::expected<UploadJob, JobError> UploadJob::create(UploadJobId id, LogFileIdentity file,
                                                     DetectedAt detected_at,
                                                     const ProviderSelection& enabled_providers) {
    if (id.value == 0) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidJobId, "Upload job ID must be non-zero"));
    }
    if (file.canonical_path.empty()) {
        return std::unexpected(make_error(JobErrorCode::EmptyPath, "Log path must not be empty"));
    }
    if (enabled_providers[provider_index(Provider::Twitch)] &&
        !enabled_providers[provider_index(Provider::DpsReport)]) {
        return std::unexpected(make_error(JobErrorCode::TwitchRequiresDpsReport,
                                          "Twitch requires dps.report to be enabled"));
    }

    return UploadJob{id, std::move(file), detected_at, enabled_providers};
}

std::expected<UploadJob, JobError> UploadJob::restore(UploadJobId id, UploadJobRecord record) {
    if (id.value == 0) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidJobId, "Upload job ID must be non-zero"));
    }
    if (record.file.canonical_path.empty()) {
        return std::unexpected(make_error(JobErrorCode::EmptyPath, "Log path must not be empty"));
    }
    if (record.dps_report_result && record.dps_report_result->permalink.empty()) {
        return std::unexpected(
            make_error(JobErrorCode::EmptyPermalink, "dps.report permalink must not be empty"));
    }
    if (record.wingman_upload_receipt && record.wingman_upload_receipt->permalink.empty()) {
        return std::unexpected(make_error(JobErrorCode::InvalidWingmanUpload,
                                          "GW2Wingman permalink must not be empty"));
    }
    if (record.donbot_upload_receipt && !valid_donbot_upload(*record.donbot_upload_receipt)) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidDonBotUpload, "DonBot upload receipt is invalid"));
    }

    ProviderSelection disabled{};
    UploadJob job{id, std::move(record.file), record.detected_at, disabled};
    job.encounter_metadata_ = std::move(record.encounter_metadata);
    job.dps_report_result_ = std::move(record.dps_report_result);
    job.wingman_upload_receipt_ = std::move(record.wingman_upload_receipt);
    job.donbot_upload_receipt_ = std::move(record.donbot_upload_receipt);
    job.twitch_delivery_receipt_ = std::move(record.twitch_delivery_receipt);
    job.providers_ = std::move(record.providers);
    for (auto& provider : job.providers_) {
        if (provider.state == ProviderState::Waiting || provider.state == ProviderState::Active ||
            provider.state == ProviderState::RetryScheduled) {
            provider.state = ProviderState::Failed;
            provider.detail = "Interrupted by the previous game session";
            provider.retry_at.reset();
        }
    }
    return job;
}

UploadJob::UploadJob(UploadJobId id, LogFileIdentity file, DetectedAt detected_at,
                     const ProviderSelection& enabled_providers)
    : id_(id), file_(std::move(file)), detected_at_(detected_at) {
    for (std::size_t index = 0; index < provider_count; ++index) {
        providers_[index].state =
            enabled_providers[index] ? ProviderState::Waiting : ProviderState::Disabled;
    }
}

UploadJobId UploadJob::id() const noexcept {
    return id_;
}

const LogFileIdentity& UploadJob::file() const noexcept {
    return file_;
}

UploadJob::DetectedAt UploadJob::detected_at() const noexcept {
    return detected_at_;
}

const ProviderStatus& UploadJob::provider_status(Provider provider) const noexcept {
    return providers_[provider_index(provider)];
}

const std::optional<EncounterMetadata>& UploadJob::encounter_metadata() const noexcept {
    return encounter_metadata_;
}

const std::optional<DpsReportResult>& UploadJob::dps_report_result() const noexcept {
    return dps_report_result_;
}

const std::optional<WingmanUploadReceipt>& UploadJob::wingman_upload_receipt() const noexcept {
    return wingman_upload_receipt_;
}

const std::optional<DonBotUploadReceipt>& UploadJob::donbot_upload_receipt() const noexcept {
    return donbot_upload_receipt_;
}

const std::optional<TwitchDeliveryReceipt>& UploadJob::twitch_delivery_receipt() const noexcept {
    return twitch_delivery_receipt_;
}

std::expected<void, JobError> UploadJob::set_encounter_metadata(EncounterMetadata metadata) {
    if (encounter_metadata_.has_value()) {
        return std::unexpected(make_error(JobErrorCode::MetadataAlreadySet,
                                          "Encounter metadata has already been set"));
    }

    encounter_metadata_ = std::move(metadata);
    return {};
}

std::expected<void, JobError> UploadJob::transition(Provider provider, ProviderState next,
                                                    std::string detail,
                                                    std::optional<RetryAt> retry_at) {
    if (!is_known_provider(provider)) {
        return std::unexpected(
            make_error(JobErrorCode::UnknownProvider, "Unknown upload provider"));
    }

    auto& status = providers_[provider_index(provider)];
    if (!is_allowed_transition(status.state, next)) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidTransition, "Invalid provider state transition"));
    }
    if (next == ProviderState::RetryScheduled && !retry_at.has_value()) {
        return std::unexpected(
            make_error(JobErrorCode::RetryTimeRequired, "A scheduled retry requires a retry time"));
    }
    if (next != ProviderState::RetryScheduled && retry_at.has_value()) {
        return std::unexpected(make_error(JobErrorCode::UnexpectedRetryTime,
                                          "Retry time is valid only for a scheduled retry"));
    }
    if (provider == Provider::DpsReport && next == ProviderState::Succeeded) {
        return std::unexpected(make_error(JobErrorCode::DpsReportResultRequired,
                                          "Complete dps.report with its response payload"));
    }
    if (provider == Provider::Twitch && next == ProviderState::Active) {
        const auto& dps_status = providers_[provider_index(Provider::DpsReport)];
        if (dps_status.state != ProviderState::Succeeded || !dps_report_result_.has_value()) {
            return std::unexpected(make_error(JobErrorCode::DpsReportResultRequired,
                                              "Twitch requires a successful dps.report result"));
        }
    }
    if (provider == Provider::Twitch && next == ProviderState::Succeeded &&
        (!twitch_delivery_receipt_ ||
         twitch_delivery_receipt_->status != TwitchDeliveryStatus::Sent)) {
        return std::unexpected(make_error(JobErrorCode::InvalidTwitchDelivery,
                                          "Twitch success requires a sent delivery receipt"));
    }

    status.state = next;
    status.detail = std::move(detail);
    status.retry_at = retry_at;
    if (next == ProviderState::Active) {
        ++status.attempts;
    }
    if (next == ProviderState::Waiting) {
        status.detail.clear();
    }

    return {};
}

std::expected<void, JobError> UploadJob::complete_dps_report(DpsReportResult result,
                                                             std::string detail) {
    auto& status = providers_[provider_index(Provider::DpsReport)];
    if (status.state != ProviderState::Active) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidTransition, "dps.report is not active"));
    }
    if (result.permalink.empty()) {
        return std::unexpected(
            make_error(JobErrorCode::EmptyPermalink, "dps.report permalink must not be empty"));
    }

    dps_report_result_ = std::move(result);
    status.state = ProviderState::Succeeded;
    status.detail = std::move(detail);
    status.retry_at.reset();
    return {};
}

std::expected<void, JobError> UploadJob::record_twitch_delivery(TwitchDeliveryReceipt receipt) {
    if (providers_[provider_index(Provider::Twitch)].state != ProviderState::Active) {
        return std::unexpected(make_error(JobErrorCode::InvalidTransition, "Twitch is not active"));
    }
    if (twitch_delivery_receipt_) {
        return std::unexpected(make_error(JobErrorCode::TwitchDeliveryAlreadyRecorded,
                                          "Twitch delivery has already been recorded"));
    }
    const bool sent = receipt.status == TwitchDeliveryStatus::Sent;
    if (!valid_twitch_delivery_status(receipt.status) ||
        sent != valid_message_id(receipt.message_id)) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidTwitchDelivery, "Twitch delivery receipt is invalid"));
    }
    twitch_delivery_receipt_ = std::move(receipt);
    return {};
}

std::expected<void, JobError> UploadJob::record_wingman_upload(WingmanUploadReceipt receipt) {
    if (providers_[provider_index(Provider::Wingman)].state != ProviderState::Active) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidTransition, "GW2Wingman is not active"));
    }
    if (wingman_upload_receipt_) {
        return std::unexpected(make_error(JobErrorCode::WingmanUploadAlreadyRecorded,
                                          "GW2Wingman upload has already been recorded"));
    }
    if (receipt.permalink.empty()) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidWingmanUpload, "GW2Wingman upload receipt is invalid"));
    }
    wingman_upload_receipt_ = std::move(receipt);
    return {};
}

std::expected<void, JobError> UploadJob::record_donbot_upload(DonBotUploadReceipt receipt) {
    if (providers_[provider_index(Provider::DonBot)].state != ProviderState::Active) {
        return std::unexpected(make_error(JobErrorCode::InvalidTransition, "DonBot is not active"));
    }
    if (donbot_upload_receipt_) {
        return std::unexpected(make_error(JobErrorCode::DonBotUploadAlreadyRecorded,
                                          "DonBot upload has already been recorded"));
    }
    if (!valid_donbot_upload(receipt)) {
        return std::unexpected(
            make_error(JobErrorCode::InvalidDonBotUpload, "DonBot upload receipt is invalid"));
    }
    donbot_upload_receipt_ = std::move(receipt);
    return {};
}

std::expected<void, JobError> UploadJob::prepare_manual_retry(Provider provider) {
    if (!is_known_provider(provider)) {
        return std::unexpected(
            make_error(JobErrorCode::UnknownProvider, "Unknown upload provider"));
    }
    if (providers_[provider_index(provider)].state != ProviderState::Failed) {
        return std::unexpected(make_error(JobErrorCode::ManualRetryRequiresFailure,
                                          "Only a failed provider can be retried manually"));
    }

    if (auto waiting = transition(provider, ProviderState::Waiting); !waiting) {
        return waiting;
    }
    if (provider == Provider::Twitch) {
        twitch_delivery_receipt_.reset();
    }
    if (provider == Provider::DpsReport && !dps_report_result_) {
        for (const auto dependent : {Provider::Wingman, Provider::DonBot, Provider::Twitch}) {
            auto& status = providers_[provider_index(dependent)];
            if (status.state == ProviderState::Skipped) {
                status.state = ProviderState::Waiting;
                status.detail.clear();
                status.retry_at.reset();
            }
        }
    }
    return {};
}

std::expected<void, JobError> UploadJob::prepare_explicit_delivery(Provider provider) {
    if (!is_known_provider(provider)) {
        return std::unexpected(
            make_error(JobErrorCode::UnknownProvider, "Unknown upload provider"));
    }
    if (!encounter_metadata_) {
        return std::unexpected(make_error(JobErrorCode::EncounterMetadataRequired,
                                          "Encounter metadata is unavailable"));
    }
    const auto state = providers_[provider_index(provider)].state;
    if (state == ProviderState::Waiting || state == ProviderState::Active ||
        state == ProviderState::RetryScheduled) {
        return std::unexpected(make_error(JobErrorCode::ExplicitDeliveryBusy,
                                          "The provider is already processing this log"));
    }
    if (provider == Provider::Twitch && !dps_report_result_) {
        return std::unexpected(make_error(JobErrorCode::DpsReportResultRequired,
                                          "Twitch requires a successful dps.report result"));
    }

    auto& status = providers_[provider_index(provider)];
    status.state = ProviderState::Waiting;
    status.detail.clear();
    status.retry_at.reset();
    if (provider == Provider::DpsReport) {
        dps_report_result_.reset();
    } else if (provider == Provider::Wingman) {
        wingman_upload_receipt_.reset();
    } else if (provider == Provider::DonBot) {
        donbot_upload_receipt_.reset();
    } else if (provider == Provider::Twitch) {
        twitch_delivery_receipt_.reset();
    }
    return {};
}

UploadJobRecord UploadJob::record() const {
    return UploadJobRecord{
        .file = file_,
        .detected_at = detected_at_,
        .encounter_metadata = encounter_metadata_,
        .dps_report_result = dps_report_result_,
        .wingman_upload_receipt = wingman_upload_receipt_,
        .donbot_upload_receipt = donbot_upload_receipt_,
        .twitch_delivery_receipt = twitch_delivery_receipt_,
        .providers = providers_,
    };
}

std::string_view provider_name(Provider provider) noexcept {
    switch (provider) {
    case Provider::DpsReport:
        return "dps.report";
    case Provider::Wingman:
        return "GW2Wingman";
    case Provider::DonBot:
        return "DonBot";
    case Provider::Twitch:
        return "Twitch";
    }

    return "Unknown";
}

bool is_terminal(ProviderState state) noexcept {
    return state == ProviderState::Disabled || state == ProviderState::Succeeded ||
           state == ProviderState::Skipped || state == ProviderState::Cancelled;
}

} // namespace manny_uploader::domain
