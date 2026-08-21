#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/log_metadata_parser.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <stop_token>

namespace manny_uploader::evtc {

[[nodiscard]] std::expected<domain::EncounterMetadata, ports::MetadataParseError>
decode_metadata(std::span<const std::byte> payload, const std::stop_token& stop_token = {});

} // namespace manny_uploader::evtc
