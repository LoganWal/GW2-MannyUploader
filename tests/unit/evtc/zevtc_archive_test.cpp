#include "manny_uploader/evtc/zevtc_archive.hpp"
#include "support/test_suite.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using ports::MetadataParseErrorCode;

struct ZipEntry {
    ZipEntry(std::string entry_name, std::vector<std::byte> entry_data)
        : name(std::move(entry_name)), data(std::move(entry_data)) {}

    std::string name;
    std::vector<std::byte> data;
    std::uint16_t flags{};
    std::uint16_t method{};
    std::optional<std::uint32_t> advertised_uncompressed_size;
    std::optional<std::uint32_t> advertised_compressed_size;
    std::optional<std::uint32_t> advertised_crc;
};

[[nodiscard]] ZipEntry with_flags(ZipEntry entry, std::uint16_t flags) {
    entry.flags = flags;
    return entry;
}

[[nodiscard]] ZipEntry with_method(ZipEntry entry, std::uint16_t method) {
    entry.method = method;
    return entry;
}

[[nodiscard]] ZipEntry with_uncompressed_size(ZipEntry entry, std::uint32_t size) {
    entry.advertised_uncompressed_size = size;
    return entry;
}

[[nodiscard]] ZipEntry with_crc(ZipEntry entry, std::uint32_t crc) {
    entry.advertised_crc = crc;
    return entry;
}

struct CentralEntry {
    const ZipEntry* source;
    std::uint32_t local_offset;
    std::uint32_t compressed_size;
    std::uint32_t uncompressed_size;
    std::uint32_t crc;
};

void append_little_endian(std::vector<std::byte>& target, std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        target.push_back(
            static_cast<std::byte>((value >> (index * 8U)) & static_cast<std::uint64_t>(0xffU)));
    }
}

void append_text(std::vector<std::byte>& target, std::string_view text) {
    for (const auto character : text) {
        target.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    append_text(result, text);
    return result;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : input) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] std::vector<std::byte> make_zip(const std::vector<ZipEntry>& entries) {
    std::vector<std::byte> archive;
    std::vector<CentralEntry> central_entries;
    central_entries.reserve(entries.size());

    for (const auto& entry : entries) {
        const auto actual_size = static_cast<std::uint32_t>(entry.data.size());
        const auto compressed_size = entry.advertised_compressed_size.value_or(actual_size);
        const auto uncompressed_size = entry.advertised_uncompressed_size.value_or(actual_size);
        const auto crc = entry.advertised_crc.value_or(crc32(entry.data));
        const auto offset = static_cast<std::uint32_t>(archive.size());

        append_little_endian(archive, 0x04034b50U, 4);
        append_little_endian(archive, 20, 2);
        append_little_endian(archive, entry.flags, 2);
        append_little_endian(archive, entry.method, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, crc, 4);
        append_little_endian(archive, compressed_size, 4);
        append_little_endian(archive, uncompressed_size, 4);
        append_little_endian(archive, entry.name.size(), 2);
        append_little_endian(archive, 0, 2);
        append_text(archive, entry.name);
        archive.insert(archive.end(), entry.data.begin(), entry.data.end());

        central_entries.push_back(CentralEntry{
            .source = &entry,
            .local_offset = offset,
            .compressed_size = compressed_size,
            .uncompressed_size = uncompressed_size,
            .crc = crc,
        });
    }

    const auto central_offset = static_cast<std::uint32_t>(archive.size());
    for (const auto& entry : central_entries) {
        append_little_endian(archive, 0x02014b50U, 4);
        append_little_endian(archive, 20, 2);
        append_little_endian(archive, 20, 2);
        append_little_endian(archive, entry.source->flags, 2);
        append_little_endian(archive, entry.source->method, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, entry.crc, 4);
        append_little_endian(archive, entry.compressed_size, 4);
        append_little_endian(archive, entry.uncompressed_size, 4);
        append_little_endian(archive, entry.source->name.size(), 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, 0, 2);
        append_little_endian(archive, 0, 4);
        append_little_endian(archive, entry.local_offset, 4);
        append_text(archive, entry.source->name);
    }

    const auto central_size = static_cast<std::uint32_t>(archive.size()) - central_offset;
    append_little_endian(archive, 0x06054b50U, 4);
    append_little_endian(archive, 0, 2);
    append_little_endian(archive, 0, 2);
    append_little_endian(archive, entries.size(), 2);
    append_little_endian(archive, entries.size(), 2);
    append_little_endian(archive, central_size, 4);
    append_little_endian(archive, central_offset, 4);
    append_little_endian(archive, 0, 2);
    return archive;
}

class TempArchive {
  public:
    explicit TempArchive(const std::vector<std::byte>& contents) {
        static std::atomic_uint64_t next_id{};
        path_ = std::filesystem::temp_directory_path() /
                ("manny-uploader-archive-test-" + std::to_string(next_id.fetch_add(1)) + ".zevtc");
        std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
        if (!stream) {
            throw std::runtime_error{"Unable to create archive test fixture"};
        }
        stream.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error{"Unable to write archive test fixture"};
        }
    }

    ~TempArchive() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(path_, error));
    }

    TempArchive(const TempArchive&) = delete;
    TempArchive& operator=(const TempArchive&) = delete;

    [[nodiscard]] domain::LogFileIdentity identity(std::uintmax_t size) const {
        return domain::LogFileIdentity{
            .canonical_path = path_,
            .size = size,
            .last_write_time = std::filesystem::last_write_time(path_),
        };
    }

    [[nodiscard]] domain::LogFileIdentity identity() const {
        return identity(std::filesystem::file_size(path_));
    }

  private:
    std::filesystem::path path_;
};

void write_little_endian(std::vector<std::byte>& target, std::size_t offset, std::uint64_t value,
                         std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        target[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & static_cast<std::uint64_t>(0xffU));
    }
}

[[nodiscard]] std::vector<std::byte> valid_evtc_payload() {
    std::vector<std::byte> payload(16, std::byte{});
    constexpr std::string_view version{"EVTC20260819"};
    for (std::size_t index = 0; index < version.size(); ++index) {
        payload[index] = static_cast<std::byte>(static_cast<unsigned char>(version[index]));
    }
    payload[12] = static_cast<std::byte>(1);
    write_little_endian(payload, 13, 0xbeef, 2);
    append_little_endian(payload, 1, 4);

    const auto agent_offset = payload.size();
    payload.resize(payload.size() + 96, std::byte{});
    write_little_endian(payload, agent_offset, 42, 8);
    constexpr std::string_view character{"Character"};
    constexpr std::string_view account{":Broadcaster.1234"};
    auto cursor = agent_offset + 28;
    for (const auto value : character) {
        payload[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(value));
    }
    payload[cursor++] = std::byte{};
    for (const auto value : account) {
        payload[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(value));
    }
    payload[cursor] = std::byte{};

    append_little_endian(payload, 0, 4);
    const auto event_offset = payload.size();
    payload.resize(payload.size() + 64, std::byte{});
    write_little_endian(payload, event_offset + 8, 42, 8);
    payload[event_offset + 56] = static_cast<std::byte>(13);
    return payload;
}

[[nodiscard]] auto extract(const std::vector<std::byte>& archive,
                           evtc::ZevtcArchiveLimits limits = {},
                           const std::stop_token& stop_token = {}) {
    TempArchive fixture{archive};
    auto reader = evtc::ZevtcArchiveReader::create(limits);
    if (!reader) {
        return std::expected<std::vector<std::byte>, ports::MetadataParseError>{
            std::unexpected(reader.error())};
    }
    return reader->extract(fixture.identity(), stop_token);
}

void configuration_tests(TestSuite& suite) {
    auto limits = evtc::ZevtcArchiveLimits{};
    limits.max_archive_bytes = 0;
    const auto archive = evtc::ZevtcArchiveReader::create(limits);
    MANNY_CHECK(suite, !archive.has_value());
    MANNY_CHECK(suite, archive.error().code == MetadataParseErrorCode::ResourceLimit);

    limits = {};
    limits.max_uncompressed_bytes = 0;
    MANNY_CHECK(suite, !evtc::ZevtcArchiveReader::create(limits).has_value());
    limits = {};
    limits.max_entries = 0;
    MANNY_CHECK(suite, !evtc::ZevtcArchiveReader::create(limits).has_value());
    limits = {};
    limits.max_filename_bytes = 0;
    MANNY_CHECK(suite, !evtc::ZevtcArchiveReader::create(limits).has_value());
    limits = {};
    limits.max_compression_ratio = 0;
    MANNY_CHECK(suite, !evtc::ZevtcArchiveReader::create(limits).has_value());
}

void successful_extraction_tests(TestSuite& suite) {
    const auto expected = bytes("EVTC payload");
    const auto archive = make_zip({
        ZipEntry{"notes.txt", bytes("ignored")},
        ZipEntry{"encounter/LOG.EVTC", expected},
    });
    const auto result = extract(archive);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result.value() == expected);
}

void file_validation_tests(TestSuite& suite) {
    const auto archive = make_zip({ZipEntry{"log.evtc", bytes("payload")}});
    TempArchive fixture{archive};
    auto reader = evtc::ZevtcArchiveReader::create();
    MANNY_CHECK(suite, reader.has_value());

    const auto changed = reader->extract(fixture.identity(archive.size() + 1U));
    MANNY_CHECK(suite, !changed.has_value());
    MANNY_CHECK(suite, changed.error().code == MetadataParseErrorCode::FileUnavailable);

    auto missing = fixture.identity();
    missing.canonical_path += ".missing";
    const auto unavailable = reader->extract(missing);
    MANNY_CHECK(suite, !unavailable.has_value());
    MANNY_CHECK(suite, unavailable.error().code == MetadataParseErrorCode::FileUnavailable);

    std::stop_source stopped;
    stopped.request_stop();
    const auto cancelled = reader->extract(fixture.identity(), stopped.get_token());
    MANNY_CHECK(suite, !cancelled.has_value());
    MANNY_CHECK(suite, cancelled.error().code == MetadataParseErrorCode::Cancelled);
}

void entry_validation_tests(TestSuite& suite) {
    const auto missing = extract(make_zip({ZipEntry{"notes.txt", bytes("x")}}));
    MANNY_CHECK(suite, !missing.has_value());
    MANNY_CHECK(suite, missing.error().code == MetadataParseErrorCode::InvalidArchive);

    const auto multiple = extract(make_zip({
        ZipEntry{"first.evtc", bytes("one")},
        ZipEntry{"second.evtc", bytes("two")},
    }));
    MANNY_CHECK(suite, !multiple.has_value());
    MANNY_CHECK(suite, multiple.error().code == MetadataParseErrorCode::InvalidArchive);

    const auto encrypted = extract(make_zip({with_flags(ZipEntry{"log.evtc", bytes("x")}, 1)}));
    MANNY_CHECK(suite, !encrypted.has_value());
    MANNY_CHECK(suite, encrypted.error().code == MetadataParseErrorCode::InvalidArchive);

    const auto unsupported = extract(make_zip({with_method(ZipEntry{"log.evtc", bytes("x")}, 99)}));
    MANNY_CHECK(suite, !unsupported.has_value());
    MANNY_CHECK(suite, unsupported.error().code == MetadataParseErrorCode::InvalidArchive);

    const auto empty = extract(make_zip({ZipEntry{"log.evtc", {}}}));
    MANNY_CHECK(suite, !empty.has_value());
    MANNY_CHECK(suite, empty.error().code == MetadataParseErrorCode::InvalidArchive);
}

void resource_limit_tests(TestSuite& suite) {
    const auto two_entries = make_zip({
        ZipEntry{"log.evtc", bytes("x")},
        ZipEntry{"notes.txt", bytes("x")},
    });
    auto limits = evtc::ZevtcArchiveLimits{};
    limits.max_entries = 1;
    const auto entry_count = extract(two_entries, limits);
    MANNY_CHECK(suite, !entry_count.has_value());
    MANNY_CHECK(suite, entry_count.error().code == MetadataParseErrorCode::ResourceLimit);

    const auto normal = make_zip({ZipEntry{"log.evtc", bytes("12345")}});
    limits = {};
    limits.max_archive_bytes = normal.size() - 1U;
    const auto archive_size = extract(normal, limits);
    MANNY_CHECK(suite, !archive_size.has_value());
    MANNY_CHECK(suite, archive_size.error().code == MetadataParseErrorCode::ResourceLimit);

    limits = {};
    limits.max_uncompressed_bytes = 4;
    const auto output_size = extract(normal, limits);
    MANNY_CHECK(suite, !output_size.has_value());
    MANNY_CHECK(suite, output_size.error().code == MetadataParseErrorCode::ResourceLimit);

    limits = {};
    limits.max_compression_ratio = 200;
    const auto ratio = extract(
        make_zip({with_method(with_uncompressed_size(ZipEntry{"log.evtc", bytes("x")}, 201), 8)}),
        limits);
    MANNY_CHECK(suite, !ratio.has_value());
    MANNY_CHECK(suite, ratio.error().code == MetadataParseErrorCode::ResourceLimit);

    limits = {};
    limits.max_filename_bytes = 5;
    const auto filename = extract(make_zip({ZipEntry{"long-name.evtc", bytes("x")}}), limits);
    MANNY_CHECK(suite, !filename.has_value());
    MANNY_CHECK(suite, filename.error().code == MetadataParseErrorCode::ResourceLimit);
}

void archive_integrity_tests(TestSuite& suite) {
    const auto bad_crc =
        extract(make_zip({with_crc(ZipEntry{"log.evtc", bytes("payload")}, 0x12345678U)}));
    MANNY_CHECK(suite, !bad_crc.has_value());
    MANNY_CHECK(suite, bad_crc.error().code == MetadataParseErrorCode::InvalidArchive);

    auto truncated = make_zip({ZipEntry{"log.evtc", bytes("payload")}});
    truncated.resize(truncated.size() - 5U);
    const auto invalid = extract(truncated);
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == MetadataParseErrorCode::InvalidArchive);
}

void metadata_reader_integration_tests(TestSuite& suite) {
    const auto archive = make_zip({ZipEntry{"nested/encounter.evtc", valid_evtc_payload()}});
    TempArchive fixture{archive};
    auto reader = evtc::ZevtcMetadataReader::create();
    MANNY_CHECK(suite, reader.has_value());
    const auto result = reader->parse(fixture.identity(), {});
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result->boss_id == 0xbeef);
    MANNY_CHECK(suite, result->pov_account == ":Broadcaster.1234");
}

} // namespace

void run_zevtc_archive_tests(TestSuite& suite) {
    configuration_tests(suite);
    successful_extraction_tests(suite);
    file_validation_tests(suite);
    entry_validation_tests(suite);
    resource_limit_tests(suite);
    archive_integrity_tests(suite);
    metadata_reader_integration_tests(suite);
}

} // namespace manny_uploader::test
