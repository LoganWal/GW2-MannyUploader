#pragma once

#include "manny_uploader/domain/upload_job.hpp"
#include "manny_uploader/ports/log_metadata_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <vector>

namespace manny_uploader::evtc {

struct ZevtcArchiveLimits {
    std::uint64_t max_archive_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t max_uncompressed_bytes{768ULL * 1024ULL * 1024ULL};
    std::uint32_t max_entries{16};
    std::uint32_t max_filename_bytes{1024};
    std::uint32_t max_compression_ratio{200};
};

class ZevtcArchiveReader {
  public:
    [[nodiscard]] static std::expected<ZevtcArchiveReader, ports::MetadataParseError>
    create(ZevtcArchiveLimits limits = {});

    [[nodiscard]] std::expected<std::vector<std::byte>, ports::MetadataParseError>
    extract(const domain::LogFileIdentity& file, const std::stop_token& stop_token = {}) const;

    [[nodiscard]] const ZevtcArchiveLimits& limits() const noexcept;

  private:
    explicit ZevtcArchiveReader(ZevtcArchiveLimits limits);

    ZevtcArchiveLimits limits_;
};

class ZevtcMetadataReader final : public ports::ILogMetadataReader {
  public:
    [[nodiscard]] static std::expected<ZevtcMetadataReader, ports::MetadataParseError>
    create(ZevtcArchiveLimits limits = {});

    [[nodiscard]] std::expected<domain::EncounterMetadata, ports::MetadataParseError>
    parse(const domain::LogFileIdentity& file, const std::stop_token& stop_token) override;

  private:
    explicit ZevtcMetadataReader(ZevtcArchiveReader archive_reader);

    ZevtcArchiveReader archive_reader_;
};

} // namespace manny_uploader::evtc
