#pragma once

#include "manny_uploader/application/upload_coordinator.hpp"
#include "manny_uploader/ports/log_metadata_parser.hpp"

#include <cstddef>

namespace manny_uploader::application {

using MetadataParseResult = ports::MetadataParseResult;

class LogIngestionCoordinator {
  public:
    LogIngestionCoordinator(UploadCoordinator& upload_coordinator,
                            ports::ILogMetadataParser& metadata_parser);

    [[nodiscard]] std::expected<domain::UploadJobId, CoordinatorError>
    submit(domain::LogFileIdentity file, const domain::ProviderSelection& enabled_providers);
    [[nodiscard]] std::expected<void, CoordinatorError> handle_result(MetadataParseResult result);
    [[nodiscard]] std::expected<std::size_t, CoordinatorError>
    drain_upload_results(std::size_t maximum_results);
    [[nodiscard]] std::size_t dispatch_due_retries();

    void cancel_all() noexcept;
    [[nodiscard]] bool is_shutting_down() const noexcept;

  private:
    UploadCoordinator& upload_coordinator_;
    ports::ILogMetadataParser& metadata_parser_;
    bool shutting_down_{};
};

} // namespace manny_uploader::application
