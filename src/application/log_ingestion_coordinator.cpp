#include "manny_uploader/application/log_ingestion_coordinator.hpp"

#include <string>
#include <utility>

namespace manny_uploader::application {

LogIngestionCoordinator::LogIngestionCoordinator(UploadCoordinator& upload_coordinator,
                                                 ports::ILogMetadataParser& metadata_parser)
    : upload_coordinator_(upload_coordinator), metadata_parser_(metadata_parser) {}

std::expected<domain::UploadJobId, CoordinatorError>
LogIngestionCoordinator::submit(domain::LogFileIdentity file,
                                const domain::ProviderSelection& enabled_providers) {
    auto pending = upload_coordinator_.add_pending_job(file, enabled_providers);
    if (!pending) {
        return std::unexpected(pending.error());
    }

    const auto id = pending.value();
    auto queued = metadata_parser_.enqueue(ports::MetadataParseRequest{
        .job_id = id,
        .file = std::move(file),
    });
    if (!queued) {
        auto failed = upload_coordinator_.fail_pending_job(
            id, std::string{"Metadata parser dispatch failed: "} + queued.error().message);
        if (!failed) {
            return std::unexpected(failed.error());
        }
    }

    return id;
}

std::expected<void, CoordinatorError>
LogIngestionCoordinator::handle_result(MetadataParseResult result) {
    if (result.metadata.has_value()) {
        return upload_coordinator_.start_pending_job(result.job_id,
                                                     std::move(result.metadata.value()));
    }

    auto failure = std::move(result.metadata.error());
    if (failure.code == ports::MetadataParseErrorCode::Cancelled) {
        return upload_coordinator_.cancel_pending_job(result.job_id, failure.message);
    }
    return upload_coordinator_.fail_pending_job(result.job_id, failure.message);
}

std::expected<std::size_t, CoordinatorError>
LogIngestionCoordinator::drain_upload_results(std::size_t maximum_results) {
    return upload_coordinator_.drain_provider_results(maximum_results);
}

std::size_t LogIngestionCoordinator::dispatch_due_retries() {
    return upload_coordinator_.dispatch_due_retries();
}

void LogIngestionCoordinator::cancel_all() noexcept {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    metadata_parser_.cancel_pending();
    upload_coordinator_.cancel_all();
}

bool LogIngestionCoordinator::is_shutting_down() const noexcept {
    return shutting_down_;
}

} // namespace manny_uploader::application
