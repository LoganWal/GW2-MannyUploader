#include "manny_uploader/config/protected_file_secret_store.hpp"

#include "manny_uploader/support/atomic_file.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace manny_uploader::config {
namespace {

constexpr std::array envelope_magic{
    std::byte{'M'}, std::byte{'N'}, std::byte{'Y'}, std::byte{'S'},
    std::byte{'E'}, std::byte{'C'}, std::byte{'R'}, std::byte{'1'},
};
constexpr std::uint32_t envelope_version = 1;
constexpr std::size_t encoded_u32_bytes = 4;
constexpr std::size_t envelope_header_bytes = envelope_magic.size() + (3 * encoded_u32_bytes);
constexpr std::size_t envelope_checksum_bytes = encoded_u32_bytes;

[[nodiscard]] ports::SecretStoreError
make_error(ports::SecretStoreErrorCode code, std::optional<ports::SecretId> id, std::string message,
           std::optional<std::uint32_t> system_error = std::nullopt) {
    return ports::SecretStoreError{
        .code = code,
        .id = id,
        .message = std::move(message),
        .system_error = system_error,
    };
}

[[nodiscard]] std::string_view record_filename(ports::SecretId id) noexcept {
    switch (id) {
    case ports::SecretId::DpsReportUserToken:
        return "dps-report-token.bin";
    case ports::SecretId::DonBotGw2ApiKey:
        return "donbot-gw2-api-key.bin";
    case ports::SecretId::TwitchOAuthSession:
        return "twitch-oauth-session.bin";
    }
    return {};
}

void append_u32(std::vector<std::byte>& destination, std::uint32_t value) {
    for (std::size_t index = 0; index < encoded_u32_bytes; ++index) {
        destination.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < encoded_u32_bytes; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t checksum(std::span<const std::byte> bytes) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    return static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, data, bytes.size()));
}

[[nodiscard]] support::SecretValue encode_envelope(ports::SecretId id,
                                                   const support::SecretValue& value) {
    std::vector<std::byte> envelope;
    envelope.reserve(envelope_header_bytes + value.size() + envelope_checksum_bytes);
    envelope.insert(envelope.end(), envelope_magic.begin(), envelope_magic.end());
    append_u32(envelope, envelope_version);
    append_u32(envelope, static_cast<std::uint32_t>(id));
    append_u32(envelope, static_cast<std::uint32_t>(value.size()));
    envelope.insert(envelope.end(), value.bytes().begin(), value.bytes().end());
    append_u32(envelope, checksum(envelope));
    return support::SecretValue{std::move(envelope)};
}

[[nodiscard]] std::expected<support::SecretValue, ports::SecretStoreError>
decode_envelope(ports::SecretId expected_id, const support::SecretValue& envelope) {
    const auto bytes = envelope.bytes();
    if (bytes.size() < envelope_header_bytes + envelope_checksum_bytes ||
        bytes.size() > envelope_header_bytes + max_secret_value_bytes + envelope_checksum_bytes) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, expected_id,
                                          "Protected credential envelope has an invalid size"));
    }
    if (!std::ranges::equal(envelope_magic, bytes.first(envelope_magic.size()))) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, expected_id,
                                          "Protected credential envelope has invalid magic"));
    }
    if (read_u32(bytes, envelope_magic.size()) != envelope_version) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, expected_id,
                                          "Protected credential envelope version is unsupported"));
    }
    const auto stored_id = read_u32(bytes, envelope_magic.size() + encoded_u32_bytes);
    if (stored_id != static_cast<std::uint32_t>(expected_id)) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, expected_id,
                                          "Protected credential record has the wrong identifier"));
    }

    const auto payload_size = read_u32(bytes, envelope_magic.size() + (2 * encoded_u32_bytes));
    const auto expected_size =
        envelope_header_bytes + static_cast<std::size_t>(payload_size) + envelope_checksum_bytes;
    if (payload_size == 0 || payload_size > max_secret_value_bytes ||
        expected_size != bytes.size()) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, expected_id,
                                          "Protected credential payload length is invalid"));
    }

    const auto stored_checksum = read_u32(bytes, bytes.size() - envelope_checksum_bytes);
    if (stored_checksum != checksum(bytes.first(bytes.size() - envelope_checksum_bytes))) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, expected_id,
                                          "Protected credential envelope checksum failed"));
    }

    const auto payload = bytes.subspan(envelope_header_bytes, payload_size);
    return support::SecretValue{std::vector<std::byte>{payload.begin(), payload.end()}};
}

[[nodiscard]] std::expected<std::vector<std::byte>, ports::SecretStoreError>
read_record(const std::filesystem::path& path, ports::SecretId id) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::ReadFailed, id,
                                          "Could not inspect the protected credential record"));
    }
    if (!exists) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::NotFound, id,
                                          "Protected credential record does not exist"));
    }

    const auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::ReadFailed, id,
                                          "Could not determine the protected record size"));
    }
    if (file_size == 0) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::CorruptRecord, id,
                                          "Protected credential record is empty"));
    }
    if (file_size > max_protected_secret_record_bytes) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::FileTooLarge, id,
                                          "Protected credential record size is invalid"));
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::ReadFailed, id,
                                          "Could not open the protected credential record"));
    }
    std::vector<std::byte> contents(static_cast<std::size_t>(file_size));
    stream.read(reinterpret_cast<char*>(contents.data()),
                static_cast<std::streamsize>(contents.size()));
    if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::ReadFailed, id,
                                          "Protected credential changed while being read"));
    }
    char extra{};
    stream.read(&extra, 1);
    if (stream.gcount() != 0 || !stream.eof()) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::ReadFailed, id,
                                          "Could not finish reading the protected credential"));
    }
    return contents;
}

[[nodiscard]] ports::SecretStoreError
map_protection_error(ports::SecretId id, const SecretProtectionError& error, bool unprotecting) {
    ports::SecretStoreErrorCode code;
    switch (error.code) {
    case SecretProtectionErrorCode::UnsupportedEnvironment:
        code = ports::SecretStoreErrorCode::UnsupportedEnvironment;
        break;
    case SecretProtectionErrorCode::InputTooLarge:
    case SecretProtectionErrorCode::OutputTooLarge:
        code = ports::SecretStoreErrorCode::FileTooLarge;
        break;
    case SecretProtectionErrorCode::ProtectionFailed:
        code = unprotecting ? ports::SecretStoreErrorCode::UnprotectionFailed
                            : ports::SecretStoreErrorCode::ProtectionFailed;
        break;
    case SecretProtectionErrorCode::UnprotectionFailed:
        code = ports::SecretStoreErrorCode::UnprotectionFailed;
        break;
    default:
        code = unprotecting ? ports::SecretStoreErrorCode::UnprotectionFailed
                            : ports::SecretStoreErrorCode::ProtectionFailed;
        break;
    }
    return make_error(code, id, error.message, error.system_error);
}

[[nodiscard]] ports::SecretStoreError map_atomic_error(ports::SecretId id,
                                                       const support::AtomicFileError& error) {
    auto code = ports::SecretStoreErrorCode::WriteFailed;
    switch (error.code) {
    case support::AtomicFileErrorCode::DirectoryCreateFailed:
        code = ports::SecretStoreErrorCode::DirectoryCreateFailed;
        break;
    case support::AtomicFileErrorCode::FileWriteFailed:
        code = ports::SecretStoreErrorCode::WriteFailed;
        break;
    case support::AtomicFileErrorCode::FlushFailed:
        code = ports::SecretStoreErrorCode::FlushFailed;
        break;
    case support::AtomicFileErrorCode::ReplaceFailed:
        code = ports::SecretStoreErrorCode::ReplaceFailed;
        break;
    case support::AtomicFileErrorCode::DeleteFailed:
        code = ports::SecretStoreErrorCode::DeleteFailed;
        break;
    }
    return make_error(code, id, error.message);
}

[[nodiscard]] ports::SecretStoreError invalid_id_error(ports::SecretId id) {
    return make_error(ports::SecretStoreErrorCode::InvalidId, id,
                      "Credential identifier is not supported");
}

} // namespace

ProtectedFileSecretStore::ProtectedFileSecretStore(std::filesystem::path directory,
                                                   std::unique_ptr<ISecretProtector> protector)
    : directory_{std::move(directory)}, protector_{std::move(protector)} {}

std::expected<ProtectedFileSecretStore, ports::SecretStoreError>
ProtectedFileSecretStore::create(std::filesystem::path directory,
                                 std::unique_ptr<ISecretProtector> protector) {
    if (directory.empty()) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::DirectoryCreateFailed,
                                          std::nullopt,
                                          "Protected credential directory must not be empty"));
    }
    if (!protector) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::UnsupportedEnvironment,
                                          std::nullopt, "A credential protector is required"));
    }
    return ProtectedFileSecretStore{std::move(directory), std::move(protector)};
}

std::expected<support::SecretValue, ports::SecretStoreError>
ProtectedFileSecretStore::load(ports::SecretId id) const {
    if (!ports::is_known_secret_id(id)) {
        return std::unexpected(invalid_id_error(id));
    }
    auto record = read_record(record_path(id), id);
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    auto envelope = protector_->unprotect(*record);
    if (!envelope) {
        return std::unexpected(map_protection_error(id, envelope.error(), true));
    }
    return decode_envelope(id, *envelope);
}

std::expected<void, ports::SecretStoreError>
ProtectedFileSecretStore::store(ports::SecretId id, const support::SecretValue& value) {
    if (!ports::is_known_secret_id(id)) {
        return std::unexpected(invalid_id_error(id));
    }
    if (value.empty()) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::EmptySecret, id,
                                          "Credential must not be empty"));
    }
    if (value.size() > max_secret_value_bytes) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::SecretTooLarge, id,
                                          "Credential exceeds the 16 KiB limit"));
    }

    const auto envelope = encode_envelope(id, value);
    auto record = protector_->protect(envelope.bytes());
    if (!record) {
        return std::unexpected(map_protection_error(id, record.error(), false));
    }
    if (record->empty() || record->size() > max_protected_secret_record_bytes) {
        return std::unexpected(make_error(ports::SecretStoreErrorCode::FileTooLarge, id,
                                          "Protected credential record size is invalid"));
    }
    auto written = support::write_file_atomically(record_path(id), *record);
    if (!written) {
        return std::unexpected(map_atomic_error(id, written.error()));
    }
    return {};
}

std::expected<void, ports::SecretStoreError> ProtectedFileSecretStore::erase(ports::SecretId id) {
    if (!ports::is_known_secret_id(id)) {
        return std::unexpected(invalid_id_error(id));
    }

    const auto path = record_path(id);
    auto temporary = support::remove_file_if_exists(support::atomic_temporary_path(path));
    if (!temporary) {
        return std::unexpected(map_atomic_error(id, temporary.error()));
    }
    auto primary = support::remove_file_if_exists(path);
    if (!primary) {
        return std::unexpected(map_atomic_error(id, primary.error()));
    }
    return {};
}

const std::filesystem::path& ProtectedFileSecretStore::directory() const noexcept {
    return directory_;
}

std::filesystem::path ProtectedFileSecretStore::record_path(ports::SecretId id) const {
    const auto filename = record_filename(id);
    return filename.empty() ? std::filesystem::path{} : directory_ / filename;
}

} // namespace manny_uploader::config
