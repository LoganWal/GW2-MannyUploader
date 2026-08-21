#include "manny_uploader/application/twitch_session.hpp"

#include "manny_uploader/providers/twitch_client.hpp"
#include "support/test_suite.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::test {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] ports::TwitchSession session(std::string access = "ACCESS-TOKEN",
                                           std::string refresh = "REFRESH-TOKEN") {
    return ports::TwitchSession{
        .credentials =
            ports::TwitchCredentialSet{
                .access_token = support::SecretValue::from_text(access),
                .refresh_token = support::SecretValue::from_text(refresh),
                .access_expires_at = std::chrono::system_clock::time_point{1'800'000'000s},
            },
        .user_id = "141981764",
        .login = "broadcaster_name",
        .scopes = {std::string{providers::twitch_chat_scope}},
    };
}

[[nodiscard]] std::string secret_text(const support::SecretValue& value) {
    const auto bytes = value.bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] support::SecretValue mutated_record(const support::SecretValue& source,
                                                  std::size_t offset, std::byte value) {
    std::vector<std::byte> bytes{source.bytes().begin(), source.bytes().end()};
    bytes[offset] = value;
    return support::SecretValue{std::move(bytes)};
}

void round_trip_and_wire_tests(TestSuite& suite) {
    auto source = session();
    auto encoded = application::encode_twitch_session(source);
    MANNY_CHECK(suite, encoded.has_value());
    if (!encoded) {
        return;
    }
    MANNY_CHECK(suite, encoded->size() < application::max_twitch_session_bytes);
    constexpr std::string_view magic = "MNYTWSN1";
    MANNY_CHECK(suite, std::ranges::equal(encoded->bytes().first(magic.size()),
                                          std::as_bytes(std::span{magic.data(), magic.size()})));
    MANNY_CHECK(suite, encoded->bytes()[8] == std::byte{1});
    MANNY_CHECK(suite, encoded->bytes()[9] == std::byte{0});
    MANNY_CHECK(suite, encoded->bytes()[20] == std::byte{12});
    MANNY_CHECK(suite, encoded->bytes()[24] == std::byte{13});
    MANNY_CHECK(suite, encoded->bytes()[28] == std::byte{9});
    MANNY_CHECK(suite, encoded->bytes()[36] == std::byte{1});

    auto decoded = application::decode_twitch_session(*encoded);
    MANNY_CHECK(suite, decoded.has_value());
    if (!decoded) {
        return;
    }
    MANNY_CHECK(suite, secret_text(decoded->credentials.access_token) == "ACCESS-TOKEN");
    MANNY_CHECK(suite, secret_text(decoded->credentials.refresh_token) == "REFRESH-TOKEN");
    MANNY_CHECK(suite, decoded->credentials.access_expires_at ==
                           std::chrono::system_clock::time_point{1'800'000'000s});
    MANNY_CHECK(suite, decoded->user_id == "141981764");
    MANNY_CHECK(suite, decoded->login == "broadcaster_name");
    MANNY_CHECK(suite, decoded->scopes == std::vector<std::string>({"user:write:chat"}));
}

void encode_validation_tests(TestSuite& suite) {
    {
        auto value = session("", "REFRESH");
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session("ACCESS", "bad token");
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session(std::string(providers::max_twitch_token_bytes + 1, 'a'), "REFRESH");
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session();
        value.user_id = "0141981764";
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session();
        value.login = "Broadcaster";
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session();
        value.scopes = {"user:write:chat", "chat:read"};
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session();
        value.credentials.access_expires_at = std::chrono::system_clock::time_point{};
        MANNY_CHECK(suite, !application::encode_twitch_session(value).has_value());
    }
    {
        auto value = session(std::string(providers::max_twitch_token_bytes, 'a'),
                             std::string(providers::max_twitch_token_bytes, 'r'));
        const auto oversized = application::encode_twitch_session(value);
        MANNY_CHECK(suite, !oversized.has_value());
        MANNY_CHECK(suite,
                    oversized.error().code == application::TwitchSessionErrorCode::ValueTooLarge);
    }
}

void decode_corruption_tests(TestSuite& suite) {
    auto encoded = application::encode_twitch_session(session());
    MANNY_CHECK(suite, encoded.has_value());
    if (!encoded) {
        return;
    }

    MANNY_CHECK(suite, !application::decode_twitch_session(support::SecretValue{}).has_value());
    MANNY_CHECK(suite,
                !application::decode_twitch_session(support::SecretValue{std::vector<std::byte>(
                                                        application::max_twitch_session_bytes + 1)})
                     .has_value());
    for (std::size_t size = 1; size < encoded->size(); ++size) {
        const auto prefix = encoded->bytes().first(size);
        support::SecretValue truncated{std::vector<std::byte>{prefix.begin(), prefix.end()}};
        MANNY_CHECK(suite, !application::decode_twitch_session(truncated).has_value());
    }

    const auto bad_magic = mutated_record(*encoded, 0, std::byte{'X'});
    MANNY_CHECK(suite, !application::decode_twitch_session(bad_magic).has_value());
    const auto future_version = mutated_record(*encoded, 8, std::byte{2});
    const auto future = application::decode_twitch_session(future_version);
    MANNY_CHECK(suite, !future.has_value());
    MANNY_CHECK(suite,
                future.error().code == application::TwitchSessionErrorCode::UnsupportedVersion);
    const auto zero_expiry = mutated_record(*encoded, 12, std::byte{0});
    auto zero_expiry_bytes =
        std::vector<std::byte>{zero_expiry.bytes().begin(), zero_expiry.bytes().end()};
    std::ranges::fill(zero_expiry_bytes.begin() + 12, zero_expiry_bytes.begin() + 20, std::byte{0});
    MANNY_CHECK(suite, !application::decode_twitch_session(
                            support::SecretValue{std::move(zero_expiry_bytes)})
                            .has_value());
    MANNY_CHECK(suite,
                !application::decode_twitch_session(mutated_record(*encoded, 20, std::byte{0xff}))
                     .has_value());
    MANNY_CHECK(suite,
                !application::decode_twitch_session(mutated_record(*encoded, 36, std::byte{2}))
                     .has_value());

    auto trailing = std::vector<std::byte>{encoded->bytes().begin(), encoded->bytes().end()};
    trailing.push_back(std::byte{0});
    MANNY_CHECK(
        suite,
        !application::decode_twitch_session(support::SecretValue{std::move(trailing)}).has_value());

    constexpr std::size_t access_offset = 40;
    MANNY_CHECK(suite, !application::decode_twitch_session(
                            mutated_record(*encoded, access_offset, std::byte{' '}))
                            .has_value());
    constexpr std::size_t user_id_offset = access_offset + 12 + 13;
    MANNY_CHECK(suite, !application::decode_twitch_session(
                            mutated_record(*encoded, user_id_offset, std::byte{'0'}))
                            .has_value());

    const auto marker = std::string{"PRIVATE-SESSION-MARKER"};
    auto marked = session(marker, "ROTATING-PRIVATE-MARKER");
    auto marked_encoded = application::encode_twitch_session(marked);
    MANNY_CHECK(suite, marked_encoded.has_value());
    auto corrupt = mutated_record(*marked_encoded, 0, std::byte{'X'});
    const auto error = application::decode_twitch_session(corrupt).error();
    MANNY_CHECK(suite, error.message.find(marker) == std::string::npos);
    MANNY_CHECK(suite, error.message.find("ROTATING-PRIVATE-MARKER") == std::string::npos);
}

} // namespace

void run_twitch_session_tests(TestSuite& suite) {
    round_trip_and_wire_tests(suite);
    encode_validation_tests(suite);
    decode_corruption_tests(suite);
}

} // namespace manny_uploader::test
