#include "manny_uploader/evtc/metadata_decoder.hpp"
#include "support/test_suite.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using ports::MetadataParseErrorCode;

constexpr std::size_t header_size = 16;
constexpr std::size_t agent_size = 96;
constexpr std::size_t skill_size = 68;
constexpr std::size_t event_size = 64;

struct AgentSpec {
    std::uint64_t address;
    std::string account;
    bool player{true};
    bool terminate_account{true};
};

struct EventSpec {
    std::uint64_t source_address;
    std::uint8_t state_change;
};

void append_little_endian(std::vector<std::byte>& target, std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        target.push_back(
            static_cast<std::byte>((value >> (index * 8U)) & static_cast<std::uint64_t>(0xffU)));
    }
}

template <std::size_t Size>
void write_little_endian(std::array<std::byte, Size>& target, std::size_t offset,
                         std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        target[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & static_cast<std::uint64_t>(0xffU));
    }
}

void append_header(std::vector<std::byte>& target, std::uint16_t boss_id,
                   std::uint8_t revision = 1) {
    const std::string_view version{"EVTC20260819"};
    for (const auto character : version) {
        target.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    target.push_back(static_cast<std::byte>(revision));
    append_little_endian(target, boss_id, 2);
    target.push_back(std::byte{});
}

void append_agent(std::vector<std::byte>& target, const AgentSpec& spec) {
    std::array<std::byte, agent_size> record{};
    write_little_endian(record, 0, spec.address, 8);
    write_little_endian(record, 8, 1, 4);
    write_little_endian(record, 12, spec.player ? 0 : 0xffffffffU, 4);

    constexpr std::string_view character_name{"Character"};
    constexpr std::size_t name_offset = 28;
    constexpr std::size_t name_size = 64;
    std::fill(record.begin() + static_cast<std::ptrdiff_t>(name_offset),
              record.begin() + static_cast<std::ptrdiff_t>(name_offset + name_size),
              static_cast<std::byte>('x'));

    std::size_t cursor = name_offset;
    for (const auto character : character_name) {
        record[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(character));
    }
    record[cursor++] = std::byte{};
    for (const auto character : spec.account) {
        if (cursor == name_offset + name_size) {
            break;
        }
        record[cursor++] = static_cast<std::byte>(static_cast<unsigned char>(character));
    }
    if (spec.terminate_account && cursor < name_offset + name_size) {
        record[cursor] = std::byte{};
    }

    target.insert(target.end(), record.begin(), record.end());
}

void append_skill(std::vector<std::byte>& target) {
    std::array<std::byte, skill_size> record{};
    write_little_endian(record, 0, 12345, 4);
    target.insert(target.end(), record.begin(), record.end());
}

void append_event(std::vector<std::byte>& target, const EventSpec& spec) {
    std::array<std::byte, event_size> record{};
    write_little_endian(record, 8, spec.source_address, 8);
    record[56] = static_cast<std::byte>(spec.state_change);
    target.insert(target.end(), record.begin(), record.end());
}

[[nodiscard]] std::vector<std::byte> payload(std::uint16_t boss_id,
                                             const std::vector<AgentSpec>& agents,
                                             const std::vector<EventSpec>& events,
                                             std::uint32_t skill_count = 0,
                                             std::uint8_t revision = 1) {
    std::vector<std::byte> result;
    append_header(result, boss_id, revision);
    append_little_endian(result, agents.size(), 4);
    for (const auto& agent : agents) {
        append_agent(result, agent);
    }
    append_little_endian(result, skill_count, 4);
    for (std::uint32_t index = 0; index < skill_count; ++index) {
        append_skill(result);
    }
    for (const auto& event : events) {
        append_event(result, event);
    }
    return result;
}

[[nodiscard]] auto decode(const std::vector<std::byte>& bytes) {
    return evtc::decode_metadata(std::span<const std::byte>{bytes});
}

void valid_payload_tests(TestSuite& suite) {
    const auto bytes = payload(0xbeef,
                               {
                                   AgentSpec{.address = 100, .account = ":Other.1111"},
                                   AgentSpec{.address = 200, .account = ":Broadcaster.1234"},
                               },
                               {
                                   EventSpec{.source_address = 100, .state_change = 0},
                                   EventSpec{.source_address = 200, .state_change = 13},
                               },
                               2);

    const auto result = decode(bytes);
    MANNY_CHECK(suite, result.has_value());
    MANNY_CHECK(suite, result->boss_id == 0xbeef);
    MANNY_CHECK(suite, result->pov_account == ":Broadcaster.1234");

    const std::string unicode_account = ":Str\xc3\xa9"
                                        "amer.5678";
    const auto unicode =
        decode(payload(123, {AgentSpec{.address = 300, .account = unicode_account}},
                       {EventSpec{.source_address = 300, .state_change = 13}}));
    MANNY_CHECK(suite, unicode.has_value());
    MANNY_CHECK(suite, unicode->pov_account == unicode_account);
}

void header_validation_tests(TestSuite& suite) {
    const auto too_short = std::vector<std::byte>(header_size - 1);
    const auto truncated = decode(too_short);
    MANNY_CHECK(suite, !truncated.has_value());
    MANNY_CHECK(suite, truncated.error().code == MetadataParseErrorCode::MalformedLog);

    auto bad_magic = payload(123, {}, {});
    bad_magic[0] = static_cast<std::byte>('X');
    const auto invalid_magic = decode(bad_magic);
    MANNY_CHECK(suite, !invalid_magic.has_value());
    MANNY_CHECK(suite, invalid_magic.error().code == MetadataParseErrorCode::UnsupportedFormat);

    const auto revision_zero = decode(payload(123, {}, {}, 0, 0));
    MANNY_CHECK(suite, !revision_zero.has_value());
    MANNY_CHECK(suite, revision_zero.error().code == MetadataParseErrorCode::UnsupportedFormat);

    const auto future_revision = decode(payload(123, {}, {}, 0, 2));
    MANNY_CHECK(suite, !future_revision.has_value());
    MANNY_CHECK(suite, future_revision.error().code == MetadataParseErrorCode::UnsupportedFormat);
}

void truncated_section_tests(TestSuite& suite) {
    const auto complete = payload(123, {AgentSpec{.address = 42, .account = ":Player.1234"}},
                                  {EventSpec{.source_address = 42, .state_change = 13}});

    for (std::size_t length = 0; length < complete.size(); ++length) {
        const auto prefix = std::span<const std::byte>{complete}.first(length);
        MANNY_CHECK(suite, !evtc::decode_metadata(prefix).has_value());
    }

    for (const auto length :
         {std::size_t{19}, std::size_t{115}, std::size_t{119}, complete.size() - 1}) {
        auto truncated = complete;
        truncated.resize(length);
        const auto result = decode(truncated);
        MANNY_CHECK(suite, !result.has_value());
        MANNY_CHECK(suite, result.error().code == MetadataParseErrorCode::MalformedLog);
    }

    auto partial_event = complete;
    partial_event.push_back(std::byte{});
    const auto result = decode(partial_event);
    MANNY_CHECK(suite, !result.has_value());
    MANNY_CHECK(suite, result.error().code == MetadataParseErrorCode::MalformedLog);
}

void count_limit_tests(TestSuite& suite) {
    std::vector<std::byte> too_many_agents;
    append_header(too_many_agents, 123);
    append_little_endian(too_many_agents, 100001, 4);
    const auto agents = decode(too_many_agents);
    MANNY_CHECK(suite, !agents.has_value());
    MANNY_CHECK(suite, agents.error().code == MetadataParseErrorCode::MalformedLog);

    std::vector<std::byte> too_many_skills;
    append_header(too_many_skills, 123);
    append_little_endian(too_many_skills, 0, 4);
    append_little_endian(too_many_skills, 65536, 4);
    const auto skills = decode(too_many_skills);
    MANNY_CHECK(suite, !skills.has_value());
    MANNY_CHECK(suite, skills.error().code == MetadataParseErrorCode::MalformedLog);
}

void missing_point_of_view_tests(TestSuite& suite) {
    const auto no_event = decode(payload(123, {AgentSpec{.address = 42, .account = ":Player.1234"}},
                                         {EventSpec{.source_address = 42, .state_change = 0}}));
    MANNY_CHECK(suite, !no_event.has_value());
    MANNY_CHECK(suite, no_event.error().code == MetadataParseErrorCode::MissingPointOfView);

    const auto unknown_agent =
        decode(payload(123, {AgentSpec{.address = 42, .account = ":Player.1234"}},
                       {EventSpec{.source_address = 99, .state_change = 13}}));
    MANNY_CHECK(suite, !unknown_agent.has_value());
    MANNY_CHECK(suite, unknown_agent.error().code == MetadataParseErrorCode::MissingPointOfView);

    const auto non_player =
        decode(payload(123, {AgentSpec{.address = 42, .account = "Npc", .player = false}},
                       {EventSpec{.source_address = 42, .state_change = 13}}));
    MANNY_CHECK(suite, !non_player.has_value());
    MANNY_CHECK(suite, non_player.error().code == MetadataParseErrorCode::MissingPointOfView);

    const auto empty_account =
        decode(payload(123, {AgentSpec{.address = 42, .account = ""}},
                       {EventSpec{.source_address = 42, .state_change = 13}}));
    MANNY_CHECK(suite, !empty_account.has_value());
    MANNY_CHECK(suite, empty_account.error().code == MetadataParseErrorCode::MissingPointOfView);
}

void malformed_account_tests(TestSuite& suite) {
    const auto unterminated = decode(payload(
        123, {AgentSpec{.address = 42, .account = ":Player.1234", .terminate_account = false}},
        {EventSpec{.source_address = 42, .state_change = 13}}));
    MANNY_CHECK(suite, !unterminated.has_value());
    MANNY_CHECK(suite, unterminated.error().code == MetadataParseErrorCode::MalformedLog);

    std::string invalid_utf8;
    invalid_utf8.push_back(static_cast<char>(0xc3));
    invalid_utf8.push_back(static_cast<char>(0x28));
    const auto invalid =
        decode(payload(123, {AgentSpec{.address = 42, .account = std::move(invalid_utf8)}},
                       {EventSpec{.source_address = 42, .state_change = 13}}));
    MANNY_CHECK(suite, !invalid.has_value());
    MANNY_CHECK(suite, invalid.error().code == MetadataParseErrorCode::MalformedLog);
}

void cancellation_tests(TestSuite& suite) {
    const auto bytes = payload(123, {AgentSpec{.address = 42, .account = ":Player.1234"}},
                               {EventSpec{.source_address = 42, .state_change = 13}});
    std::stop_source stop_source;
    stop_source.request_stop();

    const auto result =
        evtc::decode_metadata(std::span<const std::byte>{bytes}, stop_source.get_token());
    MANNY_CHECK(suite, !result.has_value());
    MANNY_CHECK(suite, result.error().code == MetadataParseErrorCode::Cancelled);
}

} // namespace

void run_evtc_metadata_decoder_tests(TestSuite& suite) {
    valid_payload_tests(suite);
    header_validation_tests(suite);
    truncated_section_tests(suite);
    count_limit_tests(suite);
    missing_point_of_view_tests(suite);
    malformed_account_tests(suite);
    cancellation_tests(suite);
}

} // namespace manny_uploader::test
