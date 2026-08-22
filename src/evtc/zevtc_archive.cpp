#include "manny_uploader/evtc/zevtc_archive.hpp"

#include "manny_uploader/evtc/metadata_decoder.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::evtc {
namespace {

constexpr std::size_t extraction_chunk_size = std::size_t{64} * 1024U;

[[nodiscard]] ports::MetadataParseError make_error(ports::MetadataParseErrorCode code,
                                                   std::string message) {
    return ports::MetadataParseError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::MetadataParseError archive_error(mz_zip_archive& archive,
                                                      std::string_view context) {
    const auto error = mz_zip_get_last_error(&archive);
    const auto* detail = mz_zip_get_error_string(error);
    auto message = std::string{context};
    if (detail != nullptr && detail[0] != '\0') {
        message += ": ";
        message += detail;
    }
    return make_error(ports::MetadataParseErrorCode::InvalidArchive, std::move(message));
}

struct FileSource {
    std::ifstream stream;
};

[[nodiscard]] std::size_t read_archive(void* opaque, mz_uint64 file_offset, void* output,
                                       std::size_t length) noexcept {
    auto& source = *static_cast<FileSource*>(opaque);
    if (file_offset > static_cast<mz_uint64>(std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return 0;
    }

    source.stream.clear();
    source.stream.seekg(static_cast<std::streamoff>(file_offset), std::ios::beg);
    if (!source.stream) {
        return 0;
    }
    source.stream.read(static_cast<char*>(output), static_cast<std::streamsize>(length));
    const auto read = source.stream.gcount();
    return read < 0 ? 0 : static_cast<std::size_t>(read);
}

class ArchiveGuard {
  public:
    explicit ArchiveGuard(mz_zip_archive& archive) : archive_(archive) {}
    ~ArchiveGuard() {
        if (archive_.m_pState != nullptr) {
            static_cast<void>(mz_zip_reader_end(&archive_));
        }
    }

    ArchiveGuard(const ArchiveGuard&) = delete;
    ArchiveGuard& operator=(const ArchiveGuard&) = delete;

  private:
    mz_zip_archive& archive_;
};

class ExtractionGuard {
  public:
    explicit ExtractionGuard(mz_zip_reader_extract_iter_state* state) : state_(state) {}
    ~ExtractionGuard() {
        if (state_ != nullptr) {
            static_cast<void>(mz_zip_reader_extract_iter_free(state_));
        }
    }

    ExtractionGuard(const ExtractionGuard&) = delete;
    ExtractionGuard& operator=(const ExtractionGuard&) = delete;

    [[nodiscard]] bool finish() noexcept {
        auto* state = std::exchange(state_, nullptr);
        return state != nullptr && mz_zip_reader_extract_iter_free(state) == MZ_TRUE;
    }

  private:
    mz_zip_reader_extract_iter_state* state_;
};

[[nodiscard]] char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] bool has_evtc_extension(std::string_view filename) noexcept {
    constexpr std::string_view extension{".evtc"};
    if (filename.size() < extension.size()) {
        return false;
    }
    const auto suffix = filename.substr(filename.size() - extension.size());
    return std::equal(suffix.begin(), suffix.end(), extension.begin(),
                      [](char left, char right) { return ascii_lower(left) == right; });
}

[[nodiscard]] bool is_extensionless_entry(std::string_view filename) noexcept {
    const auto separator = filename.find_last_of("/\\");
    const auto leaf =
        separator == std::string_view::npos ? filename : filename.substr(separator + 1);
    return !leaf.empty() && !leaf.contains('.');
}

[[nodiscard]] std::expected<std::string, ports::MetadataParseError>
entry_filename(mz_zip_archive& archive, mz_uint index, const ZevtcArchiveLimits& limits) {
    const auto required = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
    if (required == 0) {
        return std::unexpected(archive_error(archive, "Unable to read ZIP entry name"));
    }
    if (required - 1U > limits.max_filename_bytes) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "ZIP entry filename exceeds limit"));
    }

    std::vector<char> buffer;
    try {
        buffer.resize(required);
    } catch (const std::exception&) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "Unable to allocate ZIP entry filename buffer"));
    }
    const auto written = mz_zip_reader_get_filename(&archive, index, buffer.data(), required);
    if (written != required || buffer.back() != '\0') {
        return std::unexpected(archive_error(archive, "Unable to read complete ZIP entry name"));
    }
    return std::string{buffer.data(), buffer.size() - 1U};
}

[[nodiscard]] bool exceeds_ratio(mz_uint64 uncompressed, mz_uint64 compressed,
                                 std::uint32_t maximum_ratio) noexcept {
    if (compressed == 0) {
        return uncompressed != 0;
    }
    if (compressed > std::numeric_limits<mz_uint64>::max() / maximum_ratio) {
        return false;
    }
    return uncompressed > compressed * maximum_ratio;
}

struct SelectedEntry {
    mz_uint index;
    mz_zip_archive_file_stat stat;
};

[[nodiscard]] std::expected<SelectedEntry, ports::MetadataParseError>
select_evtc_entry(mz_zip_archive& archive, const ZevtcArchiveLimits& limits) {
    const auto entry_count = mz_zip_reader_get_num_files(&archive);
    if (entry_count > limits.max_entries) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "ZIP entry count exceeds limit"));
    }

    std::optional<SelectedEntry> selected;
    std::optional<SelectedEntry> extensionless_fallback;
    std::size_t regular_entry_count{};
    for (mz_uint index = 0; index < entry_count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&archive, index, &stat) != MZ_TRUE) {
            return std::unexpected(archive_error(archive, "Unable to inspect ZIP entry"));
        }
        if (stat.m_is_directory == MZ_TRUE) {
            continue;
        }
        ++regular_entry_count;

        auto filename = entry_filename(archive, index, limits);
        if (!filename) {
            return std::unexpected(filename.error());
        }
        if (!has_evtc_extension(filename.value())) {
            if (is_extensionless_entry(filename.value())) {
                extensionless_fallback = SelectedEntry{.index = index, .stat = stat};
            }
            continue;
        }
        if (selected.has_value()) {
            return std::unexpected(make_error(ports::MetadataParseErrorCode::InvalidArchive,
                                              "ZIP contains multiple EVTC entries"));
        }
        selected = SelectedEntry{.index = index, .stat = stat};
    }

    if (!selected.has_value() && regular_entry_count == 1 && extensionless_fallback.has_value()) {
        selected = extensionless_fallback;
    }
    if (!selected.has_value()) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::InvalidArchive,
                                          "ZIP does not contain one EVTC-compatible entry"));
    }
    if (selected->stat.m_is_encrypted == MZ_TRUE || selected->stat.m_is_supported != MZ_TRUE) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::InvalidArchive,
                                          "EVTC ZIP entry is encrypted or unsupported"));
    }
    if (selected->stat.m_uncomp_size == 0) {
        return std::unexpected(
            make_error(ports::MetadataParseErrorCode::InvalidArchive, "EVTC ZIP entry is empty"));
    }
    if (selected->stat.m_uncomp_size > limits.max_uncompressed_bytes ||
        exceeds_ratio(selected->stat.m_uncomp_size, selected->stat.m_comp_size,
                      limits.max_compression_ratio)) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "EVTC ZIP entry exceeds extraction limits"));
    }
    if (selected->stat.m_uncomp_size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "EVTC ZIP entry cannot fit in memory"));
    }
    return selected.value();
}

[[nodiscard]] std::expected<std::uint64_t, ports::MetadataParseError>
open_source(FileSource& source, const domain::LogFileIdentity& file,
            const ZevtcArchiveLimits& limits) {
    source.stream.open(file.canonical_path, std::ios::binary);
    if (!source.stream) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::FileUnavailable,
                                          "Unable to open .zevtc file"));
    }
    source.stream.seekg(0, std::ios::end);
    const auto end = source.stream.tellg();
    if (end < 0) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::FileUnavailable,
                                          "Unable to determine .zevtc file size"));
    }

    const auto size = static_cast<std::uint64_t>(end);
    if (size != file.size) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::FileUnavailable,
                                          ".zevtc file changed after discovery"));
    }
    if (size == 0) {
        return std::unexpected(
            make_error(ports::MetadataParseErrorCode::InvalidArchive, ".zevtc file is empty"));
    }
    if (size > limits.max_archive_bytes) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          ".zevtc archive exceeds size limit"));
    }
    source.stream.seekg(0, std::ios::beg);
    return size;
}

} // namespace

std::expected<ZevtcArchiveReader, ports::MetadataParseError>
ZevtcArchiveReader::create(ZevtcArchiveLimits limits) {
    if (limits.max_archive_bytes == 0 || limits.max_uncompressed_bytes == 0 ||
        limits.max_entries == 0 || limits.max_filename_bytes == 0 ||
        limits.max_compression_ratio == 0) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "All .zevtc archive limits must be greater than zero"));
    }
    return ZevtcArchiveReader{limits};
}

ZevtcArchiveReader::ZevtcArchiveReader(ZevtcArchiveLimits limits) : limits_(limits) {}

std::expected<std::vector<std::byte>, ports::MetadataParseError>
ZevtcArchiveReader::extract(const domain::LogFileIdentity& file,
                            const std::stop_token& stop_token) const {
    if (stop_token.stop_requested()) {
        return std::unexpected(
            make_error(ports::MetadataParseErrorCode::Cancelled, ".zevtc extraction cancelled"));
    }

    FileSource source;
    auto archive_size = open_source(source, file, limits_);
    if (!archive_size) {
        return std::unexpected(archive_size.error());
    }

    mz_zip_archive archive{};
    archive.m_pRead = read_archive;
    archive.m_pIO_opaque = &source;
    if (mz_zip_reader_init(&archive, archive_size.value(), 0) != MZ_TRUE) {
        return std::unexpected(archive_error(archive, "Unable to open .zevtc ZIP archive"));
    }
    const ArchiveGuard archive_guard{archive};

    auto selected = select_evtc_entry(archive, limits_);
    if (!selected) {
        return std::unexpected(selected.error());
    }

    std::vector<std::byte> output;
    try {
        output.resize(static_cast<std::size_t>(selected->stat.m_uncomp_size));
    } catch (const std::exception&) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::ResourceLimit,
                                          "Unable to allocate EVTC extraction buffer"));
    }
    auto* extraction = mz_zip_reader_extract_iter_new(&archive, selected->index, 0);
    if (extraction == nullptr) {
        return std::unexpected(archive_error(archive, "Unable to begin EVTC extraction"));
    }
    ExtractionGuard extraction_guard{extraction};

    std::size_t offset{};
    while (offset < output.size()) {
        if (stop_token.stop_requested()) {
            return std::unexpected(make_error(ports::MetadataParseErrorCode::Cancelled,
                                              ".zevtc extraction cancelled"));
        }
        const auto requested = std::min(extraction_chunk_size, output.size() - offset);
        const auto read =
            mz_zip_reader_extract_iter_read(extraction, output.data() + offset, requested);
        if (read == 0) {
            return std::unexpected(archive_error(archive, "Unable to extract complete EVTC entry"));
        }
        offset += read;
    }

    if (!extraction_guard.finish()) {
        return std::unexpected(archive_error(archive, "EVTC entry failed size or CRC validation"));
    }
    return output;
}

const ZevtcArchiveLimits& ZevtcArchiveReader::limits() const noexcept {
    return limits_;
}

std::expected<ZevtcMetadataReader, ports::MetadataParseError>
ZevtcMetadataReader::create(ZevtcArchiveLimits limits) {
    auto archive_reader = ZevtcArchiveReader::create(limits);
    if (!archive_reader) {
        return std::unexpected(archive_reader.error());
    }
    return ZevtcMetadataReader{archive_reader.value()};
}

ZevtcMetadataReader::ZevtcMetadataReader(ZevtcArchiveReader archive_reader)
    : archive_reader_(archive_reader) {}

std::expected<domain::EncounterMetadata, ports::MetadataParseError>
ZevtcMetadataReader::parse(const domain::LogFileIdentity& file, const std::stop_token& stop_token) {
    auto payload = archive_reader_.extract(file, stop_token);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return decode_metadata(std::span<const std::byte>{payload.value()}, stop_token);
}

} // namespace manny_uploader::evtc
