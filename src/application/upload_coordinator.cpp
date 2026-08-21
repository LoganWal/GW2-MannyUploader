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
    for (const auto provider :
         {domain::Provider::DpsReport, domain::Provider::Wingman, domain::Provider::DonBot}) {
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
    switch (outcome) {
    case UploadOutcome::Succeeded:
        if (job.provider_status(domain::Provider::Twitch).state == domain::ProviderState::Waiting) {
            dispatch(job, domain::Provider::Twitch);
        }
        break;
    case UploadOutcome::Failed:
    case UploadOutcome::Skipped:
        settle_twitch_dependency(job, domain::ProviderState::Skipped,
                                 "Skipped because dps.report failed");
        break;
    case UploadOutcome::Cancelled:
        settle_twitch_dependency(job, domain::ProviderState::Cancelled,
                                 "Cancelled because dps.report was cancelled");
        break;
    case UploadOutcome::Retry:
        break;
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

void UploadCoordinator::cancel_all() noexcept {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;

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

bool UploadCoordinator::is_shutting_down() const noexcept {
    return shutting_down_;
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
    if (jobs_.size() < history_limit_) {
        return {};
    }

    const auto settled = std::ranges::find_if(jobs_, is_settled);
    if (settled == jobs_.end()) {
        return std::unexpected(make_error(CoordinatorErrorCode::CapacityReached,
                                          "All retained upload jobs are still active"));
    }

    jobs_.erase(settled);
    return {};
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
    return {};
}

void UploadCoordinator::dispatch(domain::UploadJob& job, domain::Provider provider) {
    auto* provider_port = providers_[domain::provider_index(provider)];
    if (provider_port == nullptr) {
        [[maybe_unused]] const auto failed = job.transition(
            provider, domain::ProviderState::Failed, "Provider implementation is unavailable");
        return;
    }

    const auto active = job.transition(provider, domain::ProviderState::Active);
    if (!active) {
        return;
    }

    if (!job.encounter_metadata().has_value()) {
        [[maybe_unused]] const auto failed = job.transition(provider, domain::ProviderState::Failed,
                                                            "Encounter metadata is unavailable");
        return;
    }
    auto request = ports::UploadRequest{
        .job_id = job.id(),
        .provider = provider,
        .file = job.file(),
        .metadata = job.encounter_metadata().value_or(domain::EncounterMetadata{}),
        .dps_report_result =
            provider == domain::Provider::Twitch ? job.dps_report_result() : std::nullopt,
        .donbot_context = std::nullopt,
        .twitch_context = std::nullopt,
        .attempt = job.provider_status(provider).attempts,
    };
    if (auto queued = provider_port->enqueue(std::move(request)); !queued) {
        [[maybe_unused]] const auto failed = job.transition(provider, domain::ProviderState::Failed,
                                                            std::move(queued.error().message));
    }
}

void UploadCoordinator::settle_twitch_dependency(domain::UploadJob& job,
                                                 domain::ProviderState state, std::string detail) {
    if (job.provider_status(domain::Provider::Twitch).state == domain::ProviderState::Waiting) {
        [[maybe_unused]] const auto settled =
            job.transition(domain::Provider::Twitch, state, std::move(detail));
    }
}

} // namespace manny_uploader::application
