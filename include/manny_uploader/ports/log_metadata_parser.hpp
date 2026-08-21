#pragma once

#include "manny_uploader/domain/upload_job.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>

namespace manny_uploader::ports {

enum class MetadataParseErrorCode : std::uint8_t {
    FileUnavailable,
    InvalidArchive,
    UnsupportedFormat,
    MalformedLog,
    MissingPointOfView,
    ResourceLimit,
    Cancelled,
    Internal,
};

struct MetadataParseError {
    MetadataParseErrorCode code;
    std::string message;
};

struct MetadataParseRequest {
    domain::UploadJobId job_id;
    domain::LogFileIdentity file;
};

struct MetadataParseDispatchError {
    std::string message;
};

struct MetadataParseResult {
    domain::UploadJobId job_id;
    std::expected<domain::EncounterMetadata, MetadataParseError> metadata;
};

class ILogMetadataReader {
  public:
    virtual ~ILogMetadataReader() = default;

    [[nodiscard]] virtual std::expected<domain::EncounterMetadata, MetadataParseError>
    parse(const domain::LogFileIdentity& file, const std::stop_token& stop_token) = 0;
};

class ILogMetadataParser {
  public:
    virtual ~ILogMetadataParser() = default;

    [[nodiscard]] virtual std::expected<void, MetadataParseDispatchError>
    enqueue(MetadataParseRequest request) = 0;
    [[nodiscard]] virtual std::optional<MetadataParseResult> try_take_result() = 0;
    virtual void cancel_pending() noexcept = 0;
};

} // namespace manny_uploader::ports
