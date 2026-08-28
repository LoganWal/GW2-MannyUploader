#include "manny_uploader/config/upload_history_store.hpp"

#include "manny_uploader/support/atomic_file.hpp"
#include "manny_uploader/support/utf8.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace manny_uploader::config {
namespace {

constexpr std::uint32_t history_schema_version = 1;
constexpr std::uintmax_t maximum_history_bytes = 32U * 1024U * 1024U;
constexpr std::size_t maximum_path_bytes = 4096;
constexpr std::size_t maximum_detail_bytes = 4096;

} // namespace

namespace upload_history_detail {

struct EncodedProviderStatus {
    std::uint8_t state{};
    std::uint32_t attempts{};
    std::string detail;
};

struct EncodedMetadata {
    std::uint16_t boss_id{};
    std::string pov_account;
    std::optional<std::uint16_t> remaining_health_basis_points;
};

struct EncodedDpsReport {
    std::string permalink;
    std::string encounter_name;
    std::uint16_t boss_id{};
    std::string mode;
    bool success{};
};

struct EncodedWingmanReceipt {
    std::string permalink;
};

struct EncodedDonBotReceipt {
    std::optional<std::uint64_t> upload_id;
    std::optional<std::uint64_t> fight_log_id;
    std::uint8_t discord_delivery_outcome{};
    std::uint16_t discord_sent{};
    std::uint16_t discord_skipped{};
    std::uint16_t discord_failed{};
    std::uint16_t discord_ambiguous{};
    std::optional<std::string> guild_id;
};

struct EncodedTwitchReceipt {
    std::uint8_t status{};
    std::optional<std::string> message_id;
};

struct EncodedUploadJob {
    std::string path;
    std::uint64_t size{};
    std::int64_t last_write_time_ticks{};
    std::int64_t detected_at_milliseconds{};
    std::optional<EncodedMetadata> encounter_metadata;
    std::optional<EncodedDpsReport> dps_report_result;
    std::optional<EncodedWingmanReceipt> wingman_upload_receipt;
    std::optional<EncodedDonBotReceipt> donbot_upload_receipt;
    std::optional<EncodedTwitchReceipt> twitch_delivery_receipt;
    std::array<EncodedProviderStatus, domain::provider_count> providers;
};

struct EncodedHistory {
    std::uint32_t schema_version{history_schema_version};
    std::vector<EncodedUploadJob> logs;
};

} // namespace upload_history_detail

namespace {

using upload_history_detail::EncodedDonBotReceipt;
using upload_history_detail::EncodedDpsReport;
using upload_history_detail::EncodedHistory;
using upload_history_detail::EncodedMetadata;
using upload_history_detail::EncodedProviderStatus;
using upload_history_detail::EncodedTwitchReceipt;
using upload_history_detail::EncodedUploadJob;
using upload_history_detail::EncodedWingmanReceipt;

struct ReadOptions : glz::opts {
    bool validate_trailing_whitespace{true};
    bool error_on_unknown_keys{false};
};

struct WriteOptions : glz::opts {
    bool prettify{true};
    bool escape_control_characters{true};
    char indentation_char{' '};
    std::uint8_t indentation_width{2};
};

[[nodiscard]] UploadHistoryStoreError make_error(UploadHistoryStoreErrorCode code,
                                                 std::string message,
                                                 const std::filesystem::path& path) {
    return UploadHistoryStoreError{.code = code, .message = std::move(message), .path = path};
}

[[nodiscard]] bool valid_text(std::string_view value, std::size_t maximum) noexcept {
    return value.size() <= maximum && support::is_valid_utf8(value) &&
           std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20U && character != '\t';
           });
}

[[nodiscard]] bool valid_guild_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 19 || value.front() == '0') {
        return false;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed != 0 &&
           parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] bool valid_discord_receipt(const EncodedDonBotReceipt& receipt) noexcept {
    const auto total = static_cast<std::uint32_t>(receipt.discord_sent) + receipt.discord_skipped +
                       receipt.discord_failed + receipt.discord_ambiguous;
    if (receipt.discord_delivery_outcome >
            static_cast<std::uint8_t>(domain::DonBotDiscordDeliveryOutcome::Ambiguous) ||
        total > 4) {
        return false;
    }
    const auto populated_categories = static_cast<unsigned>(receipt.discord_sent != 0) +
                                      static_cast<unsigned>(receipt.discord_skipped != 0) +
                                      static_cast<unsigned>(receipt.discord_failed != 0) +
                                      static_cast<unsigned>(receipt.discord_ambiguous != 0);
    switch (static_cast<domain::DonBotDiscordDeliveryOutcome>(receipt.discord_delivery_outcome)) {
    case domain::DonBotDiscordDeliveryOutcome::NotRequested:
        return populated_categories == 0;
    case domain::DonBotDiscordDeliveryOutcome::Sent:
        return receipt.discord_sent != 0 && populated_categories == 1;
    case domain::DonBotDiscordDeliveryOutcome::Partial:
        return populated_categories >= 2;
    case domain::DonBotDiscordDeliveryOutcome::Skipped:
        return receipt.discord_skipped != 0 && populated_categories == 1;
    case domain::DonBotDiscordDeliveryOutcome::Failed:
        return receipt.discord_failed != 0 && populated_categories == 1;
    case domain::DonBotDiscordDeliveryOutcome::Ambiguous:
        return receipt.discord_ambiguous != 0 && populated_categories == 1;
    }
    return false;
}

[[nodiscard]] bool same_file(const domain::UploadJobRecord& left,
                             const domain::UploadJobRecord& right) noexcept {
    return left.file.canonical_path == right.file.canonical_path &&
           left.file.size == right.file.size &&
           left.file.last_write_time == right.file.last_write_time;
}

[[nodiscard]] std::filesystem::path decode_path(std::string_view encoded) {
    std::u8string utf8;
    utf8.reserve(encoded.size());
    for (const auto character : encoded) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path{utf8};
}

[[nodiscard]] std::string encode_path(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    std::string encoded;
    encoded.reserve(utf8.size());
    for (const auto character : utf8) {
        encoded.push_back(static_cast<char>(character));
    }
    return encoded;
}

[[nodiscard]] std::expected<domain::UploadJobRecord, UploadHistoryStoreError>
decode_record(const EncodedUploadJob& encoded, const std::filesystem::path& store_path) {
    if (encoded.path.empty() || !valid_text(encoded.path, maximum_path_bytes) ||
        encoded.size == 0) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                          "Upload history contains an invalid log identity",
                                          store_path));
    }
    domain::UploadJobRecord record{
        .file =
            domain::LogFileIdentity{
                .canonical_path = decode_path(encoded.path),
                .size = encoded.size,
                .last_write_time =
                    std::filesystem::file_time_type{
                        std::filesystem::file_time_type::duration{encoded.last_write_time_ticks}},
            },
        .detected_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{
            encoded.detected_at_milliseconds}},
        .encounter_metadata = std::nullopt,
        .dps_report_result = std::nullopt,
        .wingman_upload_receipt = std::nullopt,
        .donbot_upload_receipt = std::nullopt,
        .twitch_delivery_receipt = std::nullopt,
        .providers = {},
    };
    if (encoded.encounter_metadata) {
        if (!valid_text(encoded.encounter_metadata->pov_account, 256) ||
            (encoded.encounter_metadata->remaining_health_basis_points &&
             *encoded.encounter_metadata->remaining_health_basis_points > 10'000)) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains invalid encounter metadata",
                                              store_path));
        }
        record.encounter_metadata = domain::EncounterMetadata{
            .boss_id = encoded.encounter_metadata->boss_id,
            .pov_account = encoded.encounter_metadata->pov_account,
            .remaining_health_basis_points =
                encoded.encounter_metadata->remaining_health_basis_points,
        };
    }
    if (encoded.dps_report_result) {
        if (encoded.dps_report_result->permalink.empty() ||
            !valid_text(encoded.dps_report_result->permalink, 2048) ||
            !valid_text(encoded.dps_report_result->encounter_name, 256) ||
            !valid_text(encoded.dps_report_result->mode, 64)) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains an invalid report",
                                              store_path));
        }
        record.dps_report_result = domain::DpsReportResult{
            .permalink = encoded.dps_report_result->permalink,
            .encounter_name = encoded.dps_report_result->encounter_name,
            .boss_id = encoded.dps_report_result->boss_id,
            .mode = encoded.dps_report_result->mode,
            .success = encoded.dps_report_result->success,
        };
    }
    if (encoded.wingman_upload_receipt) {
        if (encoded.wingman_upload_receipt->permalink.empty() ||
            !valid_text(encoded.wingman_upload_receipt->permalink, 2048)) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains an invalid Wingman receipt",
                                              store_path));
        }
        record.wingman_upload_receipt = domain::WingmanUploadReceipt{
            .permalink = encoded.wingman_upload_receipt->permalink,
        };
    }
    if (encoded.donbot_upload_receipt) {
        if ((encoded.donbot_upload_receipt->upload_id &&
             *encoded.donbot_upload_receipt->upload_id == 0) ||
            (encoded.donbot_upload_receipt->fight_log_id &&
             (*encoded.donbot_upload_receipt->fight_log_id == 0 ||
              *encoded.donbot_upload_receipt->fight_log_id >
                  static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) ||
            (encoded.donbot_upload_receipt->guild_id &&
             !valid_guild_id(*encoded.donbot_upload_receipt->guild_id)) ||
            !valid_discord_receipt(*encoded.donbot_upload_receipt)) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains an invalid DonBot receipt",
                                              store_path));
        }
        record.donbot_upload_receipt = domain::DonBotUploadReceipt{
            .upload_id = encoded.donbot_upload_receipt->upload_id,
            .fight_log_id = encoded.donbot_upload_receipt->fight_log_id,
            .discord_delivery =
                domain::DonBotDiscordDeliveryReceipt{
                    .outcome = static_cast<domain::DonBotDiscordDeliveryOutcome>(
                        encoded.donbot_upload_receipt->discord_delivery_outcome),
                    .sent = encoded.donbot_upload_receipt->discord_sent,
                    .skipped = encoded.donbot_upload_receipt->discord_skipped,
                    .failed = encoded.donbot_upload_receipt->discord_failed,
                    .ambiguous = encoded.donbot_upload_receipt->discord_ambiguous,
                },
            .guild_id = encoded.donbot_upload_receipt->guild_id,
        };
    }
    if (encoded.twitch_delivery_receipt) {
        if (encoded.twitch_delivery_receipt->status >
                static_cast<std::uint8_t>(domain::TwitchDeliveryStatus::OtherDrop) ||
            (encoded.twitch_delivery_receipt->message_id &&
             !valid_text(*encoded.twitch_delivery_receipt->message_id, 256))) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains an invalid Twitch receipt",
                                              store_path));
        }
        record.twitch_delivery_receipt = domain::TwitchDeliveryReceipt{
            .status =
                static_cast<domain::TwitchDeliveryStatus>(encoded.twitch_delivery_receipt->status),
            .message_id = encoded.twitch_delivery_receipt->message_id,
        };
    }
    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        const auto& status = encoded.providers[index];
        if (status.state > static_cast<std::uint8_t>(domain::ProviderState::Cancelled) ||
            !valid_text(status.detail, maximum_detail_bytes)) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains an invalid provider state",
                                              store_path));
        }
        record.providers[index] = domain::ProviderStatus{
            .state = static_cast<domain::ProviderState>(status.state),
            .attempts = status.attempts,
            .detail = status.detail,
            .retry_at = std::nullopt,
        };
    }
    return record;
}

[[nodiscard]] EncodedUploadJob encode_record(const domain::UploadJobRecord& record) {
    EncodedUploadJob encoded{
        .path = encode_path(record.file.canonical_path),
        .size = static_cast<std::uint64_t>(record.file.size),
        .last_write_time_ticks =
            static_cast<std::int64_t>(record.file.last_write_time.time_since_epoch().count()),
        .detected_at_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        record.detected_at.time_since_epoch())
                                        .count(),
        .encounter_metadata = std::nullopt,
        .dps_report_result = std::nullopt,
        .wingman_upload_receipt = std::nullopt,
        .donbot_upload_receipt = std::nullopt,
        .twitch_delivery_receipt = std::nullopt,
        .providers = {},
    };
    if (record.encounter_metadata) {
        encoded.encounter_metadata = EncodedMetadata{
            .boss_id = record.encounter_metadata->boss_id,
            .pov_account = record.encounter_metadata->pov_account,
            .remaining_health_basis_points =
                record.encounter_metadata->remaining_health_basis_points,
        };
    }
    if (record.dps_report_result) {
        encoded.dps_report_result = EncodedDpsReport{
            .permalink = record.dps_report_result->permalink,
            .encounter_name = record.dps_report_result->encounter_name,
            .boss_id = record.dps_report_result->boss_id,
            .mode = record.dps_report_result->mode,
            .success = record.dps_report_result->success,
        };
    }
    if (record.wingman_upload_receipt) {
        encoded.wingman_upload_receipt = EncodedWingmanReceipt{
            .permalink = record.wingman_upload_receipt->permalink,
        };
    }
    if (record.donbot_upload_receipt) {
        encoded.donbot_upload_receipt = EncodedDonBotReceipt{
            .upload_id = record.donbot_upload_receipt->upload_id,
            .fight_log_id = record.donbot_upload_receipt->fight_log_id,
            .discord_delivery_outcome =
                static_cast<std::uint8_t>(record.donbot_upload_receipt->discord_delivery.outcome),
            .discord_sent = record.donbot_upload_receipt->discord_delivery.sent,
            .discord_skipped = record.donbot_upload_receipt->discord_delivery.skipped,
            .discord_failed = record.donbot_upload_receipt->discord_delivery.failed,
            .discord_ambiguous = record.donbot_upload_receipt->discord_delivery.ambiguous,
            .guild_id = record.donbot_upload_receipt->guild_id,
        };
    }
    if (record.twitch_delivery_receipt) {
        encoded.twitch_delivery_receipt = EncodedTwitchReceipt{
            .status = static_cast<std::uint8_t>(record.twitch_delivery_receipt->status),
            .message_id = record.twitch_delivery_receipt->message_id,
        };
    }
    for (std::size_t index = 0; index < domain::provider_count; ++index) {
        encoded.providers[index] = EncodedProviderStatus{
            .state = static_cast<std::uint8_t>(record.providers[index].state),
            .attempts = record.providers[index].attempts,
            .detail = record.providers[index].detail,
        };
    }
    return encoded;
}

[[nodiscard]] std::expected<std::vector<domain::UploadJobRecord>, UploadHistoryStoreError>
load_records(const std::filesystem::path& path, std::size_t capacity) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::FileReadFailed,
                                              "Could not inspect upload history", path));
        }
        return std::vector<domain::UploadJobRecord>{};
    }
    const auto file_size = std::filesystem::file_size(path, error);
    if (error || file_size > maximum_history_bytes) {
        return std::unexpected(make_error(file_size > maximum_history_bytes
                                              ? UploadHistoryStoreErrorCode::FileTooLarge
                                              : UploadHistoryStoreErrorCode::FileReadFailed,
                                          "Upload history could not be read", path));
    }
    std::ifstream stream{path, std::ios::binary};
    std::string document(static_cast<std::size_t>(file_size), '\0');
    if (!stream || (!document.empty() &&
                    !stream.read(document.data(), static_cast<std::streamsize>(document.size())))) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::FileReadFailed,
                                          "Upload history could not be read", path));
    }
    EncodedHistory encoded;
    if (const auto parse_error = glz::read<ReadOptions{}>(encoded, document);
        parse_error || encoded.schema_version != history_schema_version ||
        encoded.logs.size() > capacity) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::ParseFailed,
                                          "Upload history JSON is invalid", path));
    }
    std::vector<domain::UploadJobRecord> records;
    records.reserve(encoded.logs.size());
    for (const auto& item : encoded.logs) {
        auto decoded = decode_record(item, path);
        if (!decoded) {
            return std::unexpected(std::move(decoded.error()));
        }
        if (std::ranges::any_of(
                records, [&](const auto& existing) { return same_file(existing, *decoded); })) {
            return std::unexpected(make_error(UploadHistoryStoreErrorCode::ValidationFailed,
                                              "Upload history contains a duplicate log", path));
        }
        records.push_back(std::move(*decoded));
    }
    return records;
}

[[nodiscard]] std::span<const std::byte> bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span{text.data(), text.size()});
}

} // namespace

UploadHistoryStore::UploadHistoryStore(std::filesystem::path path, std::size_t capacity,
                                       std::vector<domain::UploadJobRecord> records,
                                       std::string recovery_diagnostic)
    : path_{std::move(path)}, capacity_{capacity}, records_{std::move(records)},
      recovery_diagnostic_{std::move(recovery_diagnostic)} {}

std::expected<UploadHistoryStore, UploadHistoryStoreError>
UploadHistoryStore::create(std::filesystem::path path, std::size_t capacity) {
    if (path.empty() || path.filename().empty() || capacity == 0 || capacity > 100'000) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::InvalidConfiguration,
                                          "Upload history configuration is invalid", path));
    }
    auto loaded = load_records(path, capacity);
    if (!loaded) {
        return UploadHistoryStore{
            std::move(path), capacity, {}, "Previous upload history was invalid and was ignored"};
    }
    return UploadHistoryStore{std::move(path), capacity, std::move(*loaded), {}};
}

const std::vector<domain::UploadJobRecord>& UploadHistoryStore::records() const noexcept {
    return records_;
}

std::expected<void, UploadHistoryStoreError>
UploadHistoryStore::merge_and_save(std::span<const domain::UploadJobRecord> updates) {
    auto merged = records_;
    for (const auto& update : updates) {
        const auto found = std::ranges::find_if(
            merged, [&](const auto& existing) { return same_file(existing, update); });
        if (found == merged.end()) {
            merged.push_back(update);
        } else {
            *found = update;
        }
    }
    if (merged.size() > capacity_) {
        merged.erase(merged.begin(),
                     merged.begin() + static_cast<std::ptrdiff_t>(merged.size() - capacity_));
    }
    EncodedHistory encoded;
    encoded.logs.reserve(merged.size());
    for (const auto& record : merged) {
        encoded.logs.push_back(encode_record(record));
    }
    std::string document;
    if (const auto write_error = glz::write<WriteOptions{}>(encoded, document); write_error) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::SerializeFailed,
                                          "Could not serialize upload history", path_));
    }
    document.push_back('\n');
    if (document.size() > maximum_history_bytes) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::FileTooLarge,
                                          "Serialized upload history is too large", path_));
    }
    if (auto written = support::write_file_atomically(path_, bytes(document)); !written) {
        return std::unexpected(make_error(UploadHistoryStoreErrorCode::FileWriteFailed,
                                          written.error().message, path_));
    }
    records_ = std::move(merged);
    recovery_diagnostic_.clear();
    return {};
}

const std::string& UploadHistoryStore::recovery_diagnostic() const noexcept {
    return recovery_diagnostic_;
}

} // namespace manny_uploader::config
