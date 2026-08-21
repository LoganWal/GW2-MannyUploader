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
