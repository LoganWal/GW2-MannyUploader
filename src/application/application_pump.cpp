#include "manny_uploader/application/application_pump.hpp"

#include <utility>

namespace manny_uploader::application {
namespace {

[[nodiscard]] ApplicationPumpError make_error(ApplicationPumpErrorCode code, std::string message) {
    return ApplicationPumpError{
        .code = code,
        .message = std::move(message),
        .candidate_source_error = std::nullopt,
        .discovery_error = std::nullopt,
        .coordinator_error = std::nullopt,
    };
}

[[nodiscard]] ApplicationPumpError from_candidate_error(ports::LogCandidateSourceError error) {
    auto result = make_error(ApplicationPumpErrorCode::CandidateSource, std::move(error.message));
    result.candidate_source_error = error.code;
    return result;
}

[[nodiscard]] ApplicationPumpError from_discovery_error(filesystem::LogDiscoveryError error) {
    auto result = make_error(ApplicationPumpErrorCode::Discovery, std::move(error.message));
    result.discovery_error = error.code;
    return result;
}

[[nodiscard]] ApplicationPumpError from_coordinator_error(CoordinatorError error) {
    auto result = make_error(ApplicationPumpErrorCode::Ingestion, std::move(error.message));
    result.coordinator_error = error.code;
    return result;
}

} // namespace

std::expected<ApplicationPump, ApplicationPumpError> ApplicationPump::create(
    ports::ILogCandidateSource& candidate_source, ports::ILogMetadataParser& metadata_parser,
    LogIngestionCoordinator& ingestion_coordinator, ApplicationPumpConfig config) {
    if (config.max_metadata_results_per_tick == 0 || config.max_upload_results_per_tick == 0) {
        return std::unexpected(
            make_error(ApplicationPumpErrorCode::InvalidConfiguration,
                       "Application result limits per tick must be greater than zero"));
    }

    auto discovery = filesystem::LogDiscoveryPipeline::create(config.required_matching_observations,
                                                              config.dedupe_capacity);
    if (!discovery) {
        return std::unexpected(from_discovery_error(discovery.error()));
    }
    return ApplicationPump{candidate_source, metadata_parser, ingestion_coordinator,
                           std::move(discovery.value()), config};
}

ApplicationPump::ApplicationPump(ports::ILogCandidateSource& candidate_source,
                                 ports::ILogMetadataParser& metadata_parser,
                                 LogIngestionCoordinator& ingestion_coordinator,
                                 filesystem::LogDiscoveryPipeline discovery,
                                 ApplicationPumpConfig config)
    : candidate_source_(candidate_source), metadata_parser_(metadata_parser),
      ingestion_coordinator_(ingestion_coordinator), discovery_(std::move(discovery)),
      max_metadata_results_per_tick_(config.max_metadata_results_per_tick),
      max_upload_results_per_tick_(config.max_upload_results_per_tick) {}

std::expected<ApplicationTickReport, ApplicationPumpError>
ApplicationPump::tick(const domain::ProviderSelection& enabled_providers,
                      const std::stop_token& stop_token) {
    if (shutting_down_) {
        return std::unexpected(make_error(ApplicationPumpErrorCode::ShuttingDown,
                                          "Application pump is shutting down"));
    }

    ApplicationTickReport report;
    while (report.metadata_results_handled < max_metadata_results_per_tick_) {
        auto result = metadata_parser_.try_take_result();
        if (!result.has_value()) {
            break;
        }
        if (auto handled = ingestion_coordinator_.handle_result(std::move(result.value()));
            !handled) {
            return std::unexpected(from_coordinator_error(handled.error()));
        }
        ++report.metadata_results_handled;
    }

    auto upload_results = ingestion_coordinator_.drain_upload_results(max_upload_results_per_tick_);
    if (!upload_results) {
        return std::unexpected(from_coordinator_error(std::move(upload_results.error())));
    }
    report.upload_results_handled = *upload_results;
    report.retries_dispatched = ingestion_coordinator_.dispatch_due_retries();

    auto candidates = candidate_source_.poll(stop_token);
    if (!candidates) {
        return std::unexpected(from_candidate_error(std::move(candidates.error())));
    }
    report.root_available = candidates->root_available;
    report.scan_complete = candidates->scan_complete;
    report.observations = candidates->observations.size();
    report.removed_paths = candidates->removed_paths.size();
    report.source_issues = std::move(candidates->issues);

    for (const auto& removed : candidates->removed_paths) {
        discovery_.forget(removed);
    }
    for (auto& observation : candidates->observations) {
        auto stable = discovery_.observe(std::move(observation));
        if (!stable) {
            return std::unexpected(from_discovery_error(stable.error()));
        }
        if (!stable->has_value()) {
            continue;
        }

        auto file = std::move(stable->value());
        auto submitted = ingestion_coordinator_.submit(file, enabled_providers);
        if (!submitted) {
            if (auto released = discovery_.release(file); !released) {
                auto error = from_discovery_error(released.error());
                error.message =
                    "Unable to submit stable log and release its dedupe identity: " + error.message;
                return std::unexpected(std::move(error));
            }
            return std::unexpected(from_coordinator_error(submitted.error()));
        }
        ++report.submitted_jobs;
    }

    return report;
}

void ApplicationPump::cancel_all() noexcept {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    ingestion_coordinator_.cancel_all();
}

bool ApplicationPump::is_shutting_down() const noexcept {
    return shutting_down_;
}

std::size_t ApplicationPump::tracked_candidate_count() const noexcept {
    return discovery_.tracked_count();
}

std::size_t ApplicationPump::dedupe_size() const noexcept {
    return discovery_.dedupe_size();
}

std::size_t ApplicationPump::max_metadata_results_per_tick() const noexcept {
    return max_metadata_results_per_tick_;
}

std::size_t ApplicationPump::max_upload_results_per_tick() const noexcept {
    return max_upload_results_per_tick_;
}

} // namespace manny_uploader::application
