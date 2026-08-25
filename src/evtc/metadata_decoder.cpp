#include "manny_uploader/evtc/metadata_decoder.hpp"

#include "manny_uploader/support/utf8.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::evtc {
namespace {

constexpr std::size_t header_size = 16;
constexpr std::size_t agent_count_offset = header_size;
constexpr std::size_t agent_table_offset = agent_count_offset + sizeof(std::uint32_t);
constexpr std::size_t agent_record_size = 96;
constexpr std::size_t agent_address_offset = 0;
constexpr std::size_t agent_profession_offset = 8;
constexpr std::size_t agent_elite_offset = 12;
constexpr std::size_t agent_name_offset = 28;
constexpr std::size_t agent_name_size = 64;
constexpr std::size_t skill_record_size = 68;
constexpr std::size_t combat_event_size = 64;
constexpr std::size_t event_source_offset = 8;
constexpr std::size_t event_destination_offset = 16;
constexpr std::size_t event_state_change_offset = 56;
constexpr std::uint8_t health_percentage_state_change = 8;
constexpr std::uint8_t point_of_view_state_change = 13;
constexpr std::uint8_t supported_revision = 1;
constexpr std::uint32_t max_agent_count = 100'000;
constexpr std::uint32_t max_skill_count = 65'535;
constexpr std::size_t max_combat_event_count = 10'000'000;

class ByteReader {
  public:
    explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <std::unsigned_integral Integer>
    [[nodiscard]] std::optional<Integer> read_little_endian(std::size_t offset) const noexcept {
        if (!contains(offset, sizeof(Integer))) {
            return std::nullopt;
        }

        Integer value{};
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            const auto byte = std::to_integer<std::uint8_t>(bytes_[offset + index]);
            value |= static_cast<Integer>(static_cast<Integer>(byte) << (index * 8U));
        }
        return value;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>>
    view(std::size_t offset, std::size_t length) const noexcept {
        if (!contains(offset, length)) {
            return std::nullopt;
        }
        return bytes_.subspan(offset, length);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return bytes_.size();
    }

  private:
    [[nodiscard]] bool contains(std::size_t offset, std::size_t length) const noexcept {
        return offset <= bytes_.size() && length <= bytes_.size() - offset;
    }

    std::span<const std::byte> bytes_;
};

[[nodiscard]] ports::MetadataParseError make_error(ports::MetadataParseErrorCode code,
                                                   std::string message) {
    return ports::MetadataParseError{.code = code, .message = std::move(message)};
}

[[nodiscard]] ports::MetadataParseError cancelled_error() {
    return make_error(ports::MetadataParseErrorCode::Cancelled, "EVTC metadata parsing cancelled");
}

struct TableLayout {
    std::size_t record_size;
    std::uint32_t maximum_count;
    std::string_view name;
};

[[nodiscard]] std::expected<std::size_t, ports::MetadataParseError>
checked_table_end(const ByteReader& reader, std::size_t offset, std::uint32_t count,
                  const TableLayout& layout) {
    if (count > layout.maximum_count) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          std::string{layout.name} + " count exceeds limit"));
    }
    if (offset > reader.size() ||
        static_cast<std::size_t>(count) > (reader.size() - offset) / layout.record_size) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          std::string{layout.name} + " table is truncated"));
    }
    return offset + (static_cast<std::size_t>(count) * layout.record_size);
}

[[nodiscard]] std::expected<std::string, ports::MetadataParseError>
read_account(const ByteReader& reader, std::size_t agent_offset) {
    const auto combined_name = reader.view(agent_offset + agent_name_offset, agent_name_size);
    if (!combined_name.has_value()) {
        return std::unexpected(
            make_error(ports::MetadataParseErrorCode::MalformedLog, "POV agent name is truncated"));
    }

    const auto first_nul = std::find(combined_name->begin(), combined_name->end(), std::byte{});
    if (first_nul == combined_name->end()) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "POV character name is not terminated"));
    }
    const auto account_begin = first_nul + 1;
    const auto second_nul = std::find(account_begin, combined_name->end(), std::byte{});
    if (second_nul == combined_name->end()) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "POV account name is not terminated"));
    }
    if (account_begin == second_nul) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MissingPointOfView,
                                          "POV agent has no account name"));
    }

    const auto account_length = static_cast<std::size_t>(second_nul - account_begin);
    const auto account_bytes = std::span<const std::byte>{account_begin, account_length};
    if (!support::is_valid_utf8(account_bytes)) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "POV account name is not valid UTF-8"));
    }

    std::string account;
    account.reserve(account_length);
    for (const auto byte : account_bytes) {
        account.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return account;
}

struct PayloadLayout {
    std::uint16_t boss_id;
    std::uint32_t agent_count;
    std::size_t event_table_offset;
    std::size_t event_count;
};

[[nodiscard]] std::expected<PayloadLayout, ports::MetadataParseError>
parse_payload_layout(const ByteReader& reader) {
    if (reader.size() < header_size) {
        return std::unexpected(
            make_error(ports::MetadataParseErrorCode::MalformedLog, "EVTC header is truncated"));
    }

    constexpr std::array<std::byte, 4> magic{
        static_cast<std::byte>('E'),
        static_cast<std::byte>('V'),
        static_cast<std::byte>('T'),
        static_cast<std::byte>('C'),
    };
    const auto payload_magic = reader.view(0, magic.size());
    if (!payload_magic.has_value() ||
        !std::equal(magic.begin(), magic.end(), payload_magic->begin())) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::UnsupportedFormat,
                                          "Payload does not have EVTC magic"));
    }

    const auto revision = reader.read_little_endian<std::uint8_t>(12);
    if (!revision.has_value() || revision.value() != supported_revision) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::UnsupportedFormat,
                                          "EVTC revision is not supported"));
    }
    const auto boss_id = reader.read_little_endian<std::uint16_t>(13);
    if (!boss_id.has_value()) {
        return std::unexpected(
            make_error(ports::MetadataParseErrorCode::MalformedLog, "EVTC boss ID is truncated"));
    }

    const auto agent_count = reader.read_little_endian<std::uint32_t>(agent_count_offset);
    if (!agent_count.has_value()) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "EVTC agent count is truncated"));
    }
    constexpr TableLayout agent_layout{
        .record_size = agent_record_size,
        .maximum_count = max_agent_count,
        .name = "Agent",
    };
    const auto agent_table_end =
        checked_table_end(reader, agent_table_offset, agent_count.value(), agent_layout);
    if (!agent_table_end) {
        return std::unexpected(agent_table_end.error());
    }

    const auto skill_count = reader.read_little_endian<std::uint32_t>(agent_table_end.value());
    if (!skill_count.has_value()) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "EVTC skill count is truncated"));
    }
    const auto skill_table_offset = agent_table_end.value() + sizeof(std::uint32_t);
    constexpr TableLayout skill_layout{
        .record_size = skill_record_size,
        .maximum_count = max_skill_count,
        .name = "Skill",
    };
    const auto skill_table_end =
        checked_table_end(reader, skill_table_offset, skill_count.value(), skill_layout);
    if (!skill_table_end) {
        return std::unexpected(skill_table_end.error());
    }

    const auto event_bytes = reader.size() - skill_table_end.value();
    if (event_bytes % combat_event_size != 0) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "EVTC combat event table is truncated"));
    }
    const auto event_count = event_bytes / combat_event_size;
    if (event_count > max_combat_event_count) {
        return std::unexpected(make_error(ports::MetadataParseErrorCode::MalformedLog,
                                          "Combat event count exceeds limit"));
    }

    return PayloadLayout{
        .boss_id = boss_id.value(),
        .agent_count = agent_count.value(),
        .event_table_offset = skill_table_end.value(),
        .event_count = event_count,
    };
}

[[nodiscard]] std::expected<std::uint64_t, ports::MetadataParseError>
find_point_of_view_address(const ByteReader& reader, const PayloadLayout& layout,
                           const std::stop_token& stop_token) {
    for (std::size_t index = 0; index < layout.event_count; ++index) {
        if (stop_token.stop_requested()) {
            return std::unexpected(cancelled_error());
        }

        const auto event_offset = layout.event_table_offset + (index * combat_event_size);
        const auto state_change =
            reader.read_little_endian<std::uint8_t>(event_offset + event_state_change_offset);
        if (!state_change.has_value() || state_change.value() != point_of_view_state_change) {
            continue;
        }

        const auto source =
            reader.read_little_endian<std::uint64_t>(event_offset + event_source_offset);
        if (source.has_value() && source.value() != 0) {
            return source.value();
        }
    }

    return std::unexpected(make_error(ports::MetadataParseErrorCode::MissingPointOfView,
                                      "EVTC payload has no point-of-view event"));
}

[[nodiscard]] std::expected<std::string, ports::MetadataParseError>
find_point_of_view_account(const ByteReader& reader, const PayloadLayout& layout,
                           std::uint64_t point_of_view_address, const std::stop_token& stop_token) {
    for (std::uint32_t index = 0; index < layout.agent_count; ++index) {
        if (stop_token.stop_requested()) {
            return std::unexpected(cancelled_error());
        }

        const auto agent_offset =
            agent_table_offset + (static_cast<std::size_t>(index) * agent_record_size);
        const auto address =
            reader.read_little_endian<std::uint64_t>(agent_offset + agent_address_offset);
        if (!address.has_value() || address.value() != point_of_view_address) {
            continue;
        }

        const auto elite =
            reader.read_little_endian<std::uint32_t>(agent_offset + agent_elite_offset);
        if (!elite.has_value() || elite.value() == std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(make_error(ports::MetadataParseErrorCode::MissingPointOfView,
                                              "Point-of-view address is not a player agent"));
        }
        return read_account(reader, agent_offset);
    }

    return std::unexpected(make_error(ports::MetadataParseErrorCode::MissingPointOfView,
                                      "Point-of-view agent is absent from the agent table"));
}

[[nodiscard]] std::expected<std::optional<std::uint16_t>, ports::MetadataParseError>
find_remaining_boss_health(const ByteReader& reader, const PayloadLayout& layout,
                           const std::stop_token& stop_token) {
    std::vector<std::uint64_t> boss_addresses;
    for (std::uint32_t index = 0; index < layout.agent_count; ++index) {
        if (stop_token.stop_requested()) {
            return std::unexpected(cancelled_error());
        }
        const auto agent_offset =
            agent_table_offset + (static_cast<std::size_t>(index) * agent_record_size);
        const auto profession =
            reader.read_little_endian<std::uint32_t>(agent_offset + agent_profession_offset);
        const auto elite =
            reader.read_little_endian<std::uint32_t>(agent_offset + agent_elite_offset);
        const auto address =
            reader.read_little_endian<std::uint64_t>(agent_offset + agent_address_offset);
        if (profession && elite && address && *elite == std::numeric_limits<std::uint32_t>::max() &&
            (*profession >> 16U) != std::numeric_limits<std::uint16_t>::max() &&
            static_cast<std::uint16_t>(*profession) == layout.boss_id && *address != 0) {
            boss_addresses.push_back(*address);
        }
    }

    std::optional<std::uint16_t> remaining_health;
    for (std::size_t index = 0; index < layout.event_count; ++index) {
        if (stop_token.stop_requested()) {
            return std::unexpected(cancelled_error());
        }
        const auto event_offset = layout.event_table_offset + (index * combat_event_size);
        const auto state_change =
            reader.read_little_endian<std::uint8_t>(event_offset + event_state_change_offset);
        if (!state_change || *state_change != health_percentage_state_change) {
            continue;
        }
        const auto source =
            reader.read_little_endian<std::uint64_t>(event_offset + event_source_offset);
        const auto percentage =
            reader.read_little_endian<std::uint64_t>(event_offset + event_destination_offset);
        if (source && percentage && *percentage <= 10'000 &&
            std::ranges::find(boss_addresses, *source) != boss_addresses.end()) {
            remaining_health = static_cast<std::uint16_t>(*percentage);
        }
    }
    return remaining_health;
}

} // namespace

std::expected<domain::EncounterMetadata, ports::MetadataParseError>
decode_metadata(std::span<const std::byte> payload, const std::stop_token& stop_token) {
    if (stop_token.stop_requested()) {
        return std::unexpected(cancelled_error());
    }

    const ByteReader reader{payload};
    const auto layout = parse_payload_layout(reader);
    if (!layout) {
        return std::unexpected(layout.error());
    }
    const auto point_of_view_address =
        find_point_of_view_address(reader, layout.value(), stop_token);
    if (!point_of_view_address) {
        return std::unexpected(point_of_view_address.error());
    }
    auto account = find_point_of_view_account(reader, layout.value(), point_of_view_address.value(),
                                              stop_token);
    if (!account) {
        return std::unexpected(account.error());
    }
    auto remaining_health = find_remaining_boss_health(reader, *layout, stop_token);
    if (!remaining_health) {
        return std::unexpected(remaining_health.error());
    }

    return domain::EncounterMetadata{
        .boss_id = layout->boss_id,
        .pov_account = std::move(account.value()),
        .remaining_health_basis_points = *remaining_health,
    };
}

} // namespace manny_uploader::evtc
