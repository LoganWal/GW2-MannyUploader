#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/clock.hpp"
#include "manny_uploader/ports/upload_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace manny_uploader::application {

using UploadOutcome = ports::UploadOutcome;
using UploadResult = ports::UploadResult;

struct UploadJobSnapshot {
    domain::UploadJobId id;
    domain::LogFileIdentity file;
    domain::UploadJob::DetectedAt detected_at;
    std::optional<domain::EncounterMetadata> encounter_metadata;
    std::optional<domain::DpsReportResult> dps_report_result;
    std::optional<domain::TwitchDeliveryReceipt> twitch_delivery_receipt;
    std::array<domain::ProviderStatus, domain::provider_count> providers;
};

enum class CoordinatorErrorCode : std::uint8_t {
    InvalidHistoryLimit,
    NullProvider,
    UnknownProvider,
    DuplicateProvider,
    MissingProvider,
    DomainRuleViolation,
    CapacityReached,
    JobIdExhausted,
    UnknownJob,
    UnexpectedResult,
    ShuttingDown,
};

struct CoordinatorError {
    CoordinatorErrorCode code;
    std::string message;
    std::optional<domain::JobErrorCode> domain_error;
};

class UploadCoordinator {
  public:
    [[nodiscard]] static std::expected<UploadCoordinator, CoordinatorError>
    create(ports::IClock& clock, std::span<ports::IUploadProvider* const> provider_ports,
           std::size_t history_limit = 50);

    [[nodiscard]] std::expected<domain::UploadJobId, CoordinatorError>
    add_job(domain::LogFileIdentity file, domain::EncounterMetadata metadata,
            const domain::ProviderSelection& enabled_providers);

    [[nodiscard]] std::expected<domain::UploadJobId, CoordinatorError>
    add_pending_job(domain::LogFileIdentity file,
                    const domain::ProviderSelection& enabled_providers);
    [[nodiscard]] std::expected<void, CoordinatorError>
    start_pending_job(domain::UploadJobId id, domain::EncounterMetadata metadata);
    [[nodiscard]] std::expected<void, CoordinatorError> fail_pending_job(domain::UploadJobId id,
                                                                         const std::string& detail);
    [[nodiscard]] std::expected<void, CoordinatorError>
    cancel_pending_job(domain::UploadJobId id, const std::string& detail);

    [[nodiscard]] std::expected<void, CoordinatorError> handle_result(UploadResult result);
    [[nodiscard]] std::expected<void, CoordinatorError>
    retry_failed_provider(domain::UploadJobId id, domain::Provider provider);
    [[nodiscard]] std::expected<std::size_t, CoordinatorError>
    drain_provider_results(std::size_t maximum_results);
    [[nodiscard]] std::size_t dispatch_due_retries();
    [[nodiscard]] std::expected<void, CoordinatorError>
    update_history_limit(std::size_t history_limit);
    void cancel_all() noexcept;

    [[nodiscard]] std::vector<UploadJobSnapshot> snapshots() const;
    [[nodiscard]] std::size_t history_limit() const noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    UploadCoordinator(ports::IClock& clock,
                      std::array<ports::IUploadProvider*, domain::provider_count> providers,
                      std::size_t history_limit);

    [[nodiscard]] domain::UploadJob* find_job(domain::UploadJobId id) noexcept;
    [[nodiscard]] const domain::UploadJob* find_job(domain::UploadJobId id) const noexcept;
    [[nodiscard]] std::expected<void, CoordinatorError>
    validate_selected_providers(const domain::ProviderSelection& enabled_providers) const;
    [[nodiscard]] std::expected<void, CoordinatorError> make_capacity();
    void trim_settled_history() noexcept;
    [[nodiscard]] std::expected<void, CoordinatorError>
    settle_pending_job(domain::UploadJobId id, domain::ProviderState state,
                       const std::string& detail);
    [[nodiscard]] static std::expected<void, CoordinatorError>
    validate_result(const domain::UploadJob& job, const UploadResult& result);
    [[nodiscard]] std::expected<void, CoordinatorError> apply_result(domain::UploadJob& job,
                                                                     UploadResult result);
    void dispatch_initial_providers(domain::UploadJob& job);
    void dispatch(domain::UploadJob& job, domain::Provider provider,
                  bool user_initiated_retry = false);
    void settle_dps_report_dependency(domain::UploadJob& job, UploadOutcome outcome);
    static void settle_twitch_dependency(domain::UploadJob& job, domain::ProviderState state,
                                         std::string detail);

    ports::IClock& clock_;
    std::array<ports::IUploadProvider*, domain::provider_count> providers_;
    std::size_t history_limit_;
    std::uint64_t next_job_id_{1};
    std::deque<domain::UploadJob> jobs_;
    std::size_t next_result_provider_index_{};
    bool shutting_down_{};
};

} // namespace manny_uploader::application
