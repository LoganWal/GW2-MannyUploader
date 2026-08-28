#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace manny_uploader::ports {

struct DonBotUploadContext {
    std::string api_base_url;
    std::string guild_id;
    domain::DonBotDiscordDeliveryMode discord_delivery_mode{
        domain::DonBotDiscordDeliveryMode::None};
    std::string discord_channel_id;
};

struct TwitchUploadContext {
    std::string message_template;
    bool post_success;
    bool post_failure;
};

struct UploadRequest {
    domain::UploadJobId job_id;
    domain::Provider provider;
    domain::LogFileIdentity file;
    domain::EncounterMetadata metadata;
    std::optional<domain::DpsReportResult> dps_report_result;
    std::optional<DonBotUploadContext> donbot_context;
    std::optional<TwitchUploadContext> twitch_context{};
    std::uint32_t attempt{};
    bool user_initiated_retry{};
};

struct DispatchError {
    std::string message;
};

enum class UploadOutcome : std::uint8_t {
    Succeeded,
    Skipped,
    Retry,
    Failed,
    Cancelled,
};

struct UploadResult {
    domain::UploadJobId job_id;
    domain::Provider provider;
    UploadOutcome outcome;
    std::string detail;
    std::optional<std::chrono::seconds> retry_after;
    std::optional<domain::DpsReportResult> dps_report_result;
    std::optional<domain::WingmanUploadReceipt> wingman_upload_receipt{};
    std::optional<domain::DonBotUploadReceipt> donbot_upload_receipt{};
    std::optional<domain::TwitchDeliveryReceipt> twitch_delivery_receipt{};
};

class IUploadProvider {
  public:
    virtual ~IUploadProvider() = default;

    [[nodiscard]] virtual domain::Provider provider() const noexcept = 0;
    [[nodiscard]] virtual std::expected<void, DispatchError> enqueue(UploadRequest request) = 0;
    [[nodiscard]] virtual std::optional<UploadResult> try_take_result() = 0;
    virtual void cancel_pending() noexcept = 0;
};

} // namespace manny_uploader::ports
