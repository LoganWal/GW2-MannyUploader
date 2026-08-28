#include "manny_uploader/application/upload_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace manny_uploader::application {
namespace {

[[nodiscard]] CoordinatorError make_error(CoordinatorErrorCode code, std::string message) {
    return CoordinatorError{
        .code = code, .message = std::move(message), .domain_error = std::nullopt};
}

[[nodiscard]] CoordinatorError from_domain_error(const domain::JobError& error) {
    return CoordinatorError{
        .code = CoordinatorErrorCode::DomainRuleViolation,
        .message = error.message,
        .domain_error = error.code,
    };
}

[[nodiscard]] bool is_known_provider(domain::Provider provider) noexcept {
    return domain::provider_index(provider) < domain::provider_count;
}

[[nodiscard]] bool is_settled(const domain::UploadJob& job) noexcept {
    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        const auto provider = static_cast<domain::Provider>(index);
        const auto state = job.provider_status(provider).state;
        if (state == domain::ProviderState::Waiting || state == domain::ProviderState::Active ||
            state == domain::ProviderState::RetryScheduled) {
            return false;
        }
    }

    return true;
}

} // namespace

std::expected<UploadCoordinator, CoordinatorError>
UploadCoordinator::create(ports::IClock& clock,
                          std::span<ports::IUploadProvider* const> provider_ports,
                          std::size_t history_limit) {
    if (history_limit == 0) {
        return std::unexpected(make_error(CoordinatorErrorCode::InvalidHistoryLimit,
                                          "History limit must be greater than zero"));
    }

    std::array<ports::IUploadProvider*, domain::provider_count> providers{};
    for (auto* provider : provider_ports) {
        if (provider == nullptr) {
            return std::unexpected(
                make_error(CoordinatorErrorCode::NullProvider, "Provider must not be null"));
        }
        if (!is_known_provider(provider->provider())) {
            return std::unexpected(make_error(CoordinatorErrorCode::UnknownProvider,
                                              "Provider returned an unknown ID"));
        }

        const auto index = domain::provider_index(provider->provider());
        if (providers[index] != nullptr) {
            return std::unexpected(make_error(CoordinatorErrorCode::DuplicateProvider,
                                              "Provider was registered more than once"));
        }
        providers[index] = provider;
    }

    return UploadCoordinator{clock, providers, history_limit};
}

UploadCoordinator::UploadCoordinator(
    ports::IClock& clock, std::array<ports::IUploadProvider*, domain::provider_count> providers,
    std::size_t history_limit)
    : clock_(clock), providers_(providers), history_limit_(history_limit) {}

std::expected<domain::UploadJobId, CoordinatorError>
UploadCoordinator::add_job(domain::LogFileIdentity file, domain::EncounterMetadata metadata,
                           const domain::ProviderSelection& enabled_providers) {
    auto pending = add_pending_job(std::move(file), enabled_providers);
    if (!pending) {
        return std::unexpected(pending.error());
    }

    const auto id = pending.value();
    if (auto started = start_pending_job(id, std::move(metadata)); !started) {
        return std::unexpected(started.error());
    }
    return id;
}

std::expected<domain::UploadJobId, CoordinatorError>
UploadCoordinator::add_pending_job(domain::LogFileIdentity file,
                                   const domain::ProviderSelection& enabled_providers) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    if (next_job_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::JobIdExhausted, "Upload job IDs are exhausted"));
    }
    if (const auto validation = validate_selected_providers(enabled_providers); !validation) {
        return std::unexpected(validation.error());
    }
    if (const auto capacity = make_capacity(); !capacity) {
        return std::unexpected(capacity.error());
    }

    const auto id = domain::UploadJobId{next_job_id_++};
    auto created =
        domain::UploadJob::create(id, std::move(file), clock_.system_now(), enabled_providers);
    if (!created) {
        return std::unexpected(from_domain_error(created.error()));
    }

    jobs_.push_back(std::move(created.value()));
    return id;
}

std::expected<void, CoordinatorError>
UploadCoordinator::start_pending_job(domain::UploadJobId id, domain::EncounterMetadata metadata) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }

    auto* job = find_job(id);
    if (job == nullptr) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownJob, "Metadata references an unknown job"));
    }
    if (job->encounter_metadata().has_value()) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Job metadata has already been supplied"));
    }
    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        const auto provider = static_cast<domain::Provider>(index);
        const auto state = job->provider_status(provider).state;
        if (state != domain::ProviderState::Waiting && state != domain::ProviderState::Disabled) {
            return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                              "Job is no longer waiting for metadata"));
        }
    }

    if (const auto metadata_result = job->set_encounter_metadata(std::move(metadata));
        !metadata_result) {
        return std::unexpected(from_domain_error(metadata_result.error()));
    }
    dispatch_initial_providers(*job);
    trim_settled_history();
    return {};
}

std::expected<void, CoordinatorError>
UploadCoordinator::fail_pending_job(domain::UploadJobId id, const std::string& detail) {
    return settle_pending_job(id, domain::ProviderState::Failed, detail);
}

std::expected<void, CoordinatorError>
UploadCoordinator::cancel_pending_job(domain::UploadJobId id, const std::string& detail) {
    return settle_pending_job(id, domain::ProviderState::Cancelled, detail);
}

void UploadCoordinator::dispatch_initial_providers(domain::UploadJob& job) {
    if (job.encounter_metadata()->boss_id == domain::wvw_boss_id &&
        job.provider_status(domain::Provider::Wingman).state == domain::ProviderState::Waiting) {
        [[maybe_unused]] const auto skipped =
            job.transition(domain::Provider::Wingman, domain::ProviderState::Skipped,
                           "WvW logs are not uploaded to GW2Wingman");
    }

    if (job.provider_status(domain::Provider::DpsReport).state == domain::ProviderState::Waiting) {
        dispatch(job, domain::Provider::DpsReport);
        return;
    }

    for (const auto provider : {domain::Provider::Wingman, domain::Provider::DonBot}) {
        if (job.provider_status(provider).state == domain::ProviderState::Waiting) {
            dispatch(job, provider);
        }
    }
}

std::expected<void, CoordinatorError> UploadCoordinator::handle_result(UploadResult result) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    if (!is_known_provider(result.provider)) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownProvider, "Result has an unknown provider"));
    }

    auto* job = find_job(result.job_id);
    if (job == nullptr) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownJob, "Result references an unknown job"));
    }
    if (auto valid = validate_result(*job, result); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    const auto provider = result.provider;
    const auto outcome = result.outcome;
    if (auto applied = apply_result(*job, std::move(result)); !applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (provider == domain::Provider::DpsReport) {
        settle_dps_report_dependency(*job, outcome);
    }
    trim_settled_history();
    return {};
}

std::expected<void, CoordinatorError>
UploadCoordinator::retry_failed_provider(domain::UploadJobId id, domain::Provider provider) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    if (!is_known_provider(provider)) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownProvider, "Retry uses an unknown provider"));
    }

    auto* job = find_job(id);
    if (job == nullptr) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownJob, "Retry references an unknown job"));
    }
    if (!job->encounter_metadata()) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnexpectedResult,
                       "A metadata failure must be retried by detecting the log again"));
    }
    if (auto prepared = job->prepare_manual_retry(provider); !prepared) {
        return std::unexpected(from_domain_error(prepared.error()));
    }

    dispatch(*job, provider, true);
    const auto& status = job->provider_status(provider);
    if (status.state == domain::ProviderState::Failed) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnexpectedResult,
                       status.detail.empty() ? "Unable to queue the retry" : status.detail));
    }
    return {};
}

std::expected<void, CoordinatorError> UploadCoordinator::reupload(domain::UploadJobId id) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    auto* job = find_job(id);
    if (job == nullptr) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownJob, "Reupload references an unknown job"));
    }
    auto prepared = *job;
    for (const auto provider :
         {domain::Provider::DpsReport, domain::Provider::Wingman, domain::Provider::DonBot}) {
        if (auto result = prepared.prepare_explicit_delivery(provider); !result) {
            return std::unexpected(from_domain_error(result.error()));
        }
    }
    *job = std::move(prepared);
    explicit_reuploads_.push_back(job->id().value);
    dispatch(*job, domain::Provider::DpsReport, true);
    return {};
}

std::expected<void, CoordinatorError> UploadCoordinator::rechat(domain::UploadJobId id) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    auto* job = find_job(id);
    if (job == nullptr) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownJob, "Rechat references an unknown job"));
    }
    if (auto prepared = job->prepare_explicit_delivery(domain::Provider::Twitch); !prepared) {
        return std::unexpected(from_domain_error(prepared.error()));
    }
    dispatch(*job, domain::Provider::Twitch, true);
    return {};
}

std::expected<void, CoordinatorError>
UploadCoordinator::restore_history(std::span<const domain::UploadJobRecord> records) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    if (!jobs_.empty()) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Upload history must be restored before new jobs"));
    }
    const auto start = records.size() > history_limit_ ? records.size() - history_limit_ : 0;
    for (std::size_t index = start; index < records.size(); ++index) {
        if (next_job_id_ == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(
                make_error(CoordinatorErrorCode::JobIdExhausted, "Upload job IDs are exhausted"));
        }
        auto restored =
            domain::UploadJob::restore(domain::UploadJobId{next_job_id_}, records[index]);
        if (!restored) {
            return std::unexpected(from_domain_error(restored.error()));
        }
        ++next_job_id_;
        jobs_.push_back(std::move(*restored));
    }
    return {};
}

std::expected<void, CoordinatorError>
UploadCoordinator::validate_result(const domain::UploadJob& job, const UploadResult& result) {
    if (job.provider_status(result.provider).state != domain::ProviderState::Active) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Provider is not active for this job"));
    }
    if (result.outcome != UploadOutcome::Retry && result.retry_after.has_value()) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Only a retry result may include a retry delay"));
    }
    if (result.outcome != UploadOutcome::Succeeded && result.dps_report_result.has_value()) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnexpectedResult,
                       "Only a successful dps.report result may include a report response"));
    }
    if (result.wingman_upload_receipt && (result.provider != domain::Provider::Wingman ||
                                          result.outcome != UploadOutcome::Succeeded)) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnexpectedResult,
                       "Only a successful GW2Wingman upload may include a Wingman receipt"));
    }
    if (result.donbot_upload_receipt && (result.provider != domain::Provider::DonBot ||
                                         result.outcome != UploadOutcome::Succeeded)) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnexpectedResult,
                       "Only a successful DonBot upload may include a DonBot receipt"));
    }
    if (result.twitch_delivery_receipt && result.provider != domain::Provider::Twitch) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Only Twitch may include a delivery receipt"));
    }
    if (result.twitch_delivery_receipt) {
        const bool sent =
            result.twitch_delivery_receipt->status == domain::TwitchDeliveryStatus::Sent;
        if ((result.outcome == UploadOutcome::Succeeded) != sent ||
            (result.outcome != UploadOutcome::Succeeded &&
             result.outcome != UploadOutcome::Failed)) {
            return std::unexpected(
                make_error(CoordinatorErrorCode::UnexpectedResult,
                           "Twitch delivery receipt does not match its outcome"));
        }
    }
    if (result.provider == domain::Provider::Twitch && result.outcome == UploadOutcome::Succeeded &&
        !result.twitch_delivery_receipt) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Twitch success requires a sent delivery receipt"));
    }
    if (result.outcome == UploadOutcome::Retry &&
        (!result.retry_after.has_value() || *result.retry_after <= std::chrono::seconds::zero() ||
         *result.retry_after > std::chrono::hours{24})) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Retry result requires a bounded positive delay"));
    }
    if (result.outcome == UploadOutcome::Succeeded) {
        const auto is_dps_report = result.provider == domain::Provider::DpsReport;
        if (is_dps_report != result.dps_report_result.has_value()) {
            return std::unexpected(
                make_error(CoordinatorErrorCode::UnexpectedResult,
                           "dps.report success and report response must be supplied together"));
        }
    }
    return {};
}

std::expected<void, CoordinatorError> UploadCoordinator::apply_result(domain::UploadJob& job,
                                                                      UploadResult result) {
    std::expected<void, domain::JobError> transition_result;
    switch (result.outcome) {
    case UploadOutcome::Succeeded:
        if (result.provider == domain::Provider::DpsReport) {
            if (!result.dps_report_result.has_value()) {
                return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                                  "dps.report success requires its response"));
            }
            transition_result = job.complete_dps_report(std::move(*result.dps_report_result),
                                                        std::move(result.detail));
        } else if (result.provider == domain::Provider::Wingman && result.wingman_upload_receipt) {
            transition_result =
                job.record_wingman_upload(std::move(*result.wingman_upload_receipt));
            if (transition_result) {
                transition_result = job.transition(
                    result.provider, domain::ProviderState::Succeeded, std::move(result.detail));
            }
        } else if (result.provider == domain::Provider::Twitch) {
            if (!result.twitch_delivery_receipt) {
                return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                                  "Twitch success requires a delivery receipt"));
            }
            transition_result =
                job.record_twitch_delivery(std::move(*result.twitch_delivery_receipt));
            if (transition_result) {
                transition_result = job.transition(
                    result.provider, domain::ProviderState::Succeeded, std::move(result.detail));
            }
        } else if (result.provider == domain::Provider::DonBot && result.donbot_upload_receipt) {
            transition_result = job.record_donbot_upload(std::move(*result.donbot_upload_receipt));
            if (transition_result) {
                transition_result = job.transition(
                    result.provider, domain::ProviderState::Succeeded, std::move(result.detail));
            }
        } else {
            transition_result = job.transition(result.provider, domain::ProviderState::Succeeded,
                                               std::move(result.detail));
        }
        break;
    case UploadOutcome::Skipped:
        transition_result = job.transition(result.provider, domain::ProviderState::Skipped,
                                           std::move(result.detail));
        break;
    case UploadOutcome::Retry:
        transition_result = job.transition(
            result.provider, domain::ProviderState::RetryScheduled, std::move(result.detail),
            clock_.steady_now() + result.retry_after.value_or(std::chrono::seconds::zero()));
        break;
    case UploadOutcome::Failed:
        if (result.twitch_delivery_receipt) {
            transition_result =
                job.record_twitch_delivery(std::move(*result.twitch_delivery_receipt));
            if (!transition_result) {
                break;
            }
        }
        transition_result = job.transition(result.provider, domain::ProviderState::Failed,
                                           std::move(result.detail));
        break;
    case UploadOutcome::Cancelled:
        transition_result = job.transition(result.provider, domain::ProviderState::Cancelled,
                                           std::move(result.detail));
        break;
    }

    if (!transition_result) {
        return std::unexpected(from_domain_error(transition_result.error()));
    }

    return {};
}

void UploadCoordinator::settle_dps_report_dependency(domain::UploadJob& job,
                                                     UploadOutcome outcome) {
    const auto explicit_reupload =
        std::ranges::find(explicit_reuploads_, job.id().value) != explicit_reuploads_.end();
    switch (outcome) {
    case UploadOutcome::Succeeded:
        for (const auto provider :
             {domain::Provider::Wingman, domain::Provider::DonBot, domain::Provider::Twitch}) {
            if (job.provider_status(provider).state == domain::ProviderState::Waiting) {
                dispatch(job, provider, explicit_reupload);
            }
        }
        break;
    case UploadOutcome::Failed:
    case UploadOutcome::Skipped:
        settle_dps_report_dependents(job, domain::ProviderState::Skipped,
                                     "Skipped because dps.report failed");
        break;
    case UploadOutcome::Cancelled:
        settle_dps_report_dependents(job, domain::ProviderState::Cancelled,
                                     "Cancelled because dps.report was cancelled");
        break;
    case UploadOutcome::Retry:
        break;
    }
    if (outcome != UploadOutcome::Retry) {
        std::erase(explicit_reuploads_, job.id().value);
    }
}

std::expected<std::size_t, CoordinatorError>
UploadCoordinator::drain_provider_results(std::size_t maximum_results) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }

    std::size_t handled{};
    std::size_t consecutive_empty_providers{};
    while (handled < maximum_results && consecutive_empty_providers < domain::provider_count) {
        auto* provider = providers_[next_result_provider_index_];
        next_result_provider_index_ = (next_result_provider_index_ + 1) % domain::provider_count;
        if (provider == nullptr) {
            ++consecutive_empty_providers;
            continue;
        }

        std::optional<ports::UploadResult> result;
        try {
            result = provider->try_take_result();
        } catch (...) {
            return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                              "Provider result retrieval failed"));
        }
        if (!result.has_value()) {
            ++consecutive_empty_providers;
            continue;
        }
        consecutive_empty_providers = 0;
        if (auto applied = handle_result(std::move(*result)); !applied) {
            return std::unexpected(std::move(applied.error()));
        }
        ++handled;
    }
    return handled;
}

std::size_t UploadCoordinator::dispatch_due_retries() {
    if (shutting_down_) {
        return 0;
    }

    const auto now = clock_.steady_now();
    std::size_t dispatched{};
    for (auto& job : jobs_) {
        for (std::size_t index = 0; index < domain::provider_count; ++index) {
            const auto provider = static_cast<domain::Provider>(index);
            const auto& status = job.provider_status(provider);
            if (status.state == domain::ProviderState::RetryScheduled &&
                status.retry_at.has_value() && *status.retry_at <= now) {
                dispatch(job, provider);
                ++dispatched;
            }
        }
    }

    return dispatched;
}

std::expected<void, CoordinatorError>
UploadCoordinator::update_history_limit(std::size_t history_limit) {
    if (history_limit == 0) {
        return std::unexpected(make_error(CoordinatorErrorCode::InvalidHistoryLimit,
                                          "History limit must be greater than zero"));
    }
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }
    history_limit_ = history_limit;
    trim_settled_history();
    return {};
}

void UploadCoordinator::cancel_all() noexcept {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    explicit_reuploads_.clear();

    for (auto* provider : providers_) {
        if (provider != nullptr) {
            provider->cancel_pending();
        }
    }

    for (auto& job : jobs_) {
        for (std::size_t index = 0; index < domain::provider_count; ++index) {
            const auto provider = static_cast<domain::Provider>(index);
            const auto state = job.provider_status(provider).state;
            if (state == domain::ProviderState::Waiting || state == domain::ProviderState::Active ||
                state == domain::ProviderState::RetryScheduled) {
                [[maybe_unused]] const auto cancelled =
                    job.transition(provider, domain::ProviderState::Cancelled);
            }
        }
    }
}

std::vector<UploadJobSnapshot> UploadCoordinator::snapshots() const {
    std::vector<UploadJobSnapshot> result;
    result.reserve(jobs_.size());

    for (const auto& job : jobs_) {
        UploadJobSnapshot snapshot{
            .id = job.id(),
            .file = job.file(),
            .detected_at = job.detected_at(),
            .encounter_metadata = job.encounter_metadata(),
            .dps_report_result = job.dps_report_result(),
            .wingman_upload_receipt = job.wingman_upload_receipt(),
            .donbot_upload_receipt = job.donbot_upload_receipt(),
            .twitch_delivery_receipt = job.twitch_delivery_receipt(),
            .providers = {},
        };
        for (std::size_t index = 0; index < domain::provider_count; ++index) {
            const auto provider = static_cast<domain::Provider>(index);
            snapshot.providers[index] = job.provider_status(provider);
        }
        result.push_back(std::move(snapshot));
    }

    return result;
}

std::vector<domain::UploadJobRecord> UploadCoordinator::history_records() const {
    std::vector<domain::UploadJobRecord> result;
    result.reserve(jobs_.size());
    for (const auto& job : jobs_) {
        result.push_back(job.record());
    }
    return result;
}

bool UploadCoordinator::is_shutting_down() const noexcept {
    return shutting_down_;
}

std::size_t UploadCoordinator::history_limit() const noexcept {
    return history_limit_;
}

domain::UploadJob* UploadCoordinator::find_job(domain::UploadJobId id) noexcept {
    const auto found =
        std::ranges::find_if(jobs_, [id](const auto& job) { return job.id() == id; });
    return found == jobs_.end() ? nullptr : &*found;
}

const domain::UploadJob* UploadCoordinator::find_job(domain::UploadJobId id) const noexcept {
    const auto found =
        std::ranges::find_if(jobs_, [id](const auto& job) { return job.id() == id; });
    return found == jobs_.end() ? nullptr : &*found;
}

std::expected<void, CoordinatorError> UploadCoordinator::validate_selected_providers(
    const domain::ProviderSelection& enabled_providers) const {
    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        if (enabled_providers[index] && providers_[index] == nullptr) {
            return std::unexpected(make_error(CoordinatorErrorCode::MissingProvider,
                                              "Enabled provider has no registered implementation"));
        }
    }

    return {};
}

std::expected<void, CoordinatorError> UploadCoordinator::make_capacity() {
    while (jobs_.size() >= history_limit_) {
        const auto settled = std::ranges::find_if(jobs_, is_settled);
        if (settled == jobs_.end()) {
            return std::unexpected(make_error(CoordinatorErrorCode::CapacityReached,
                                              "All retained upload jobs are still active"));
        }
        jobs_.erase(settled);
    }
    return {};
}

void UploadCoordinator::trim_settled_history() noexcept {
    while (jobs_.size() > history_limit_) {
        const auto settled = std::ranges::find_if(jobs_, is_settled);
        if (settled == jobs_.end()) {
            return;
        }
        jobs_.erase(settled);
    }
}

std::expected<void, CoordinatorError>
UploadCoordinator::settle_pending_job(domain::UploadJobId id, domain::ProviderState state,
                                      const std::string& detail) {
    if (shutting_down_) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::ShuttingDown, "Coordinator is shutting down"));
    }

    auto* job = find_job(id);
    if (job == nullptr) {
        return std::unexpected(
            make_error(CoordinatorErrorCode::UnknownJob, "Metadata references an unknown job"));
    }
    if (job->encounter_metadata().has_value()) {
        return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                          "Job is no longer waiting for metadata"));
    }
    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        const auto provider = static_cast<domain::Provider>(index);
        const auto provider_state = job->provider_status(provider).state;
        if (provider_state != domain::ProviderState::Waiting &&
            provider_state != domain::ProviderState::Disabled) {
            return std::unexpected(make_error(CoordinatorErrorCode::UnexpectedResult,
                                              "Job is no longer waiting for metadata"));
        }
    }

    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        const auto provider = static_cast<domain::Provider>(index);
        if (job->provider_status(provider).state == domain::ProviderState::Waiting) {
            if (auto settled = job->transition(provider, state, detail); !settled) {
                return std::unexpected(from_domain_error(settled.error()));
            }
        }
    }
    trim_settled_history();
    return {};
}

void UploadCoordinator::dispatch(domain::UploadJob& job, domain::Provider provider,
                                 bool user_initiated_retry) {
    if (provider == domain::Provider::Wingman && job.encounter_metadata() &&
        job.encounter_metadata()->boss_id == domain::wvw_boss_id) {
        [[maybe_unused]] const auto skipped = job.transition(
            provider, domain::ProviderState::Skipped, "WvW logs are not uploaded to GW2Wingman");
        return;
    }

    auto* provider_port = providers_[domain::provider_index(provider)];
    if (provider_port == nullptr) {
        [[maybe_unused]] const auto failed = job.transition(
            provider, domain::ProviderState::Failed, "Provider implementation is unavailable");
        if (provider == domain::Provider::DpsReport) {
            settle_dps_report_dependency(job, UploadOutcome::Failed);
        }
        return;
    }

    const auto active = job.transition(provider, domain::ProviderState::Active);
    if (!active) {
        return;
    }

    if (!job.encounter_metadata().has_value()) {
        [[maybe_unused]] const auto failed = job.transition(provider, domain::ProviderState::Failed,
                                                            "Encounter metadata is unavailable");
        if (provider == domain::Provider::DpsReport) {
            settle_dps_report_dependency(job, UploadOutcome::Failed);
        }
        return;
    }
    auto request = ports::UploadRequest{
        .job_id = job.id(),
        .provider = provider,
        .file = job.file(),
        .metadata = job.encounter_metadata().value_or(domain::EncounterMetadata{}),
        .dps_report_result = provider == domain::Provider::Wingman ||
                                     provider == domain::Provider::DonBot ||
                                     provider == domain::Provider::Twitch
                                 ? job.dps_report_result()
                                 : std::nullopt,
        .dps_report_context = std::nullopt,
        .donbot_context = std::nullopt,
        .twitch_context = std::nullopt,
        .attempt = job.provider_status(provider).attempts,
        .user_initiated_retry = user_initiated_retry,
    };
    if (auto queued = provider_port->enqueue(std::move(request)); !queued) {
        [[maybe_unused]] const auto failed = job.transition(provider, domain::ProviderState::Failed,
                                                            std::move(queued.error().message));
    }
    if (provider == domain::Provider::DpsReport &&
        job.provider_status(provider).state == domain::ProviderState::Failed) {
        settle_dps_report_dependency(job, UploadOutcome::Failed);
    }
}

void UploadCoordinator::settle_dps_report_dependents(domain::UploadJob& job,
                                                     domain::ProviderState state,
                                                     const std::string& detail) {
    for (const auto provider :
         {domain::Provider::Wingman, domain::Provider::DonBot, domain::Provider::Twitch}) {
        if (job.provider_status(provider).state == domain::ProviderState::Waiting) {
            [[maybe_unused]] const auto settled = job.transition(provider, state, detail);
        }
    }
}

} // namespace manny_uploader::application
