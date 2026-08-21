#pragma once

#include "manny_uploader/application/log_ingestion_coordinator.hpp"
#include "manny_uploader/filesystem/log_discovery.hpp"
#include "manny_uploader/ports/log_candidate_source.hpp"
#include "manny_uploader/ports/log_metadata_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace manny_uploader::application {

enum class ApplicationPumpErrorCode : std::uint8_t {
    InvalidConfiguration,
    CandidateSource,
    Discovery,
    Ingestion,
    ShuttingDown,
};

struct ApplicationPumpError {
    ApplicationPumpErrorCode code;
    std::string message;
    std::optional<ports::LogCandidateSourceErrorCode> candidate_source_error;
    std::optional<filesystem::LogDiscoveryErrorCode> discovery_error;
    std::optional<CoordinatorErrorCode> coordinator_error;
};

struct ApplicationTickReport {
    bool root_available{};
    bool scan_complete{};
    std::size_t observations{};
    std::size_t removed_paths{};
    std::size_t submitted_jobs{};
    std::size_t metadata_results_handled{};
    std::size_t upload_results_handled{};
    std::size_t retries_dispatched{};
    std::vector<ports::LogCandidateIssue> source_issues;
};

struct ApplicationPumpConfig {
    std::size_t required_matching_observations{2};
    std::size_t dedupe_capacity{256};
    std::size_t max_metadata_results_per_tick{8};
    std::size_t max_upload_results_per_tick{8};
};

class ApplicationPump {
  public:
    [[nodiscard]] static std::expected<ApplicationPump, ApplicationPumpError>
    create(ports::ILogCandidateSource& candidate_source, ports::ILogMetadataParser& metadata_parser,
           LogIngestionCoordinator& ingestion_coordinator, ApplicationPumpConfig config = {});

    [[nodiscard]] std::expected<ApplicationTickReport, ApplicationPumpError>
    tick(const domain::ProviderSelection& enabled_providers,
         const std::stop_token& stop_token = {});

    void cancel_all() noexcept;

    [[nodiscard]] bool is_shutting_down() const noexcept;
    [[nodiscard]] std::size_t tracked_candidate_count() const noexcept;
    [[nodiscard]] std::size_t dedupe_size() const noexcept;
    [[nodiscard]] std::size_t max_metadata_results_per_tick() const noexcept;
    [[nodiscard]] std::size_t max_upload_results_per_tick() const noexcept;

  private:
    ApplicationPump(ports::ILogCandidateSource& candidate_source,
                    ports::ILogMetadataParser& metadata_parser,
                    LogIngestionCoordinator& ingestion_coordinator,
                    filesystem::LogDiscoveryPipeline discovery, ApplicationPumpConfig config);

    ports::ILogCandidateSource& candidate_source_;
    ports::ILogMetadataParser& metadata_parser_;
    LogIngestionCoordinator& ingestion_coordinator_;
    filesystem::LogDiscoveryPipeline discovery_;
    std::size_t max_metadata_results_per_tick_;
    std::size_t max_upload_results_per_tick_;
    bool shutting_down_{};
};

} // namespace manny_uploader::application
