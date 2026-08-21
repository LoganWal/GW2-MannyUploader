#include "manny_uploader/application/twitch_session.hpp"

#include "manny_uploader/providers/twitch_client.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manny_uploader::application {
namespace {

constexpr std::array<std::byte, 8> session_magic{
    std::byte{'M'}, std::byte{'N'}, std::byte{'Y'}, std::byte{'T'},
    std::byte{'W'}, std::byte{'S'}, std::byte{'N'}, std::byte{'1'},
};
constexpr std::int64_t maximum_unix_seconds = 253'402'300'799;
constexpr std::size_t fixed_header_bytes = session_magic.size() + sizeof(std::uint32_t) +
                                           sizeof(std::int64_t) + (5U * sizeof(std::uint32_t));

class BufferWiper {
  public:
    explicit BufferWiper(std::vector<std::byte>& value) noexcept : value_{value} {}
    ~BufferWiper() {
        support::secure_erase(value_);
    }

    BufferWiper(const BufferWiper&) = delete;
    BufferWiper& operator=(const BufferWiper&) = delete;

  private:
    std::vector<std::byte>& value_;
};

[[nodiscard]] TwitchSessionError make_error(TwitchSessionErrorCode code, std::string message) {
    return TwitchSessionError{.code = code, .message = std::move(message)};
}

[[nodiscard]] bool valid_token(std::span<const std::byte> value) noexcept {
    return !value.empty() && value.size() <= providers::max_twitch_token_bytes &&
           std::ranges::all_of(value, [](std::byte byte) {
               const auto character = std::to_integer<unsigned char>(byte);
               return character >= 0x21U && character <= 0x7eU;
           });
}

[[nodiscard]] bool valid_user_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 20 || value.front() == '0') {
        return false;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed > 0;
}

[[nodiscard]] bool valid_login(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_';
    });
}

[[nodiscard]] bool valid_scopes(const std::vector<std::string>& scopes) noexcept {
    return scopes.size() == 1 && scopes.front() == providers::twitch_chat_scope;
}

[[nodiscard]] std::optional<std::int64_t>
expiry_seconds(std::chrono::system_clock::time_point value) noexcept {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
    if (seconds <= 0 || seconds > maximum_unix_seconds) {
        return std::nullopt;
    }
    return seconds;
}

[[nodiscard]] bool checked_add(std::size_t& total, std::size_t amount) noexcept {
    if (amount > max_twitch_session_bytes || total > max_twitch_session_bytes - amount) {
        return false;
    }
    total += amount;
    return true;
}

void append_u32(std::vector<std::byte>& destination, std::uint32_t value) {
    for (std::size_t shift = 0; shift < sizeof(value); ++shift) {
        destination.push_back(
            static_cast<std::byte>((value >> static_cast<unsigned int>(shift * 8U)) & 0xffU));
    }
}

void append_i64(std::vector<std::byte>& destination, std::int64_t value) {
    auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t shift = 0; shift < sizeof(bits); ++shift) {
        destination.push_back(
            static_cast<std::byte>((bits >> static_cast<unsigned int>(shift * 8U)) & 0xffU));
    }
}

void append_bytes(std::vector<std::byte>& destination, std::span<const std::byte> value) {
    destination.insert(destination.end(), value.begin(), value.end());
}

void append_text(std::vector<std::byte>& destination, std::string_view value) {
    append_bytes(destination, std::as_bytes(std::span{value.data(), value.size()}));
}

class Reader {
  public:
    explicit Reader(std::span<const std::byte> source) noexcept : source_{source} {}

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
        if (remaining() < sizeof(value)) {
            return false;
        }
        value = 0;
        for (std::size_t shift = 0; shift < sizeof(value); ++shift) {
            value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(source_[offset_++]))
                     << static_cast<unsigned int>(shift * 8U);
        }
        return true;
    }

    [[nodiscard]] bool read_i64(std::int64_t& value) noexcept {
        if (remaining() < sizeof(value)) {
            return false;
        }
        std::uint64_t bits{};
        for (std::size_t shift = 0; shift < sizeof(bits); ++shift) {
            bits |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(source_[offset_++]))
                    << static_cast<unsigned int>(shift * 8U);
        }
        value = std::bit_cast<std::int64_t>(bits);
        return true;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> read_bytes(std::size_t size) noexcept {
        if (size > remaining()) {
            return std::nullopt;
        }
        const auto result = source_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return source_.size() - offset_;
    }

  private:
    std::span<const std::byte> source_;
    std::size_t offset_{};
};

[[nodiscard]] std::string text_from_bytes(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

} // namespace

std::expected<support::SecretValue, TwitchSessionError>
encode_twitch_session(const ports::TwitchSession& session) {
    const auto expiry = expiry_seconds(session.credentials.access_expires_at);
    if (!valid_token(session.credentials.access_token.bytes()) ||
        !valid_token(session.credentials.refresh_token.bytes()) || !expiry ||
        !valid_user_id(session.user_id) || !valid_login(session.login) ||
        !valid_scopes(session.scopes)) {
        return std::unexpected(make_error(TwitchSessionErrorCode::InvalidSession,
                                          "The Twitch session cannot be persisted"));
    }

    std::size_t payload_size = fixed_header_bytes;
    const auto add_field = [&payload_size](std::size_t size) {
        return size <= std::numeric_limits<std::uint32_t>::max() && checked_add(payload_size, size);
    };
    if (!add_field(session.credentials.access_token.size()) ||
        !add_field(session.credentials.refresh_token.size()) ||
        !add_field(session.user_id.size()) || !add_field(session.login.size()) ||
        !checked_add(payload_size, sizeof(std::uint32_t)) ||
        !add_field(session.scopes.front().size())) {
        return std::unexpected(make_error(TwitchSessionErrorCode::ValueTooLarge,
                                          "The Twitch session is too large to persist"));
    }

    try {
        std::vector<std::byte> payload;
        BufferWiper payload_wiper{payload};
        payload.reserve(payload_size);
        append_bytes(payload, session_magic);
        append_u32(payload, current_twitch_session_version);
        append_i64(payload, *expiry);
        append_u32(payload, static_cast<std::uint32_t>(session.credentials.access_token.size()));
        append_u32(payload, static_cast<std::uint32_t>(session.credentials.refresh_token.size()));
        append_u32(payload, static_cast<std::uint32_t>(session.user_id.size()));
        append_u32(payload, static_cast<std::uint32_t>(session.login.size()));
        append_u32(payload, static_cast<std::uint32_t>(session.scopes.size()));
        append_bytes(payload, session.credentials.access_token.bytes());
        append_bytes(payload, session.credentials.refresh_token.bytes());
        append_text(payload, session.user_id);
        append_text(payload, session.login);
        for (const auto& scope : session.scopes) {
            append_u32(payload, static_cast<std::uint32_t>(scope.size()));
            append_text(payload, scope);
        }
        if (payload.size() != payload_size) {
            return std::unexpected(make_error(TwitchSessionErrorCode::InvalidSession,
                                              "The Twitch session could not be encoded"));
        }
        return support::SecretValue{std::move(payload)};
    } catch (...) {
        return std::unexpected(make_error(TwitchSessionErrorCode::InvalidSession,
                                          "The Twitch session could not be encoded"));
    }
}

std::expected<ports::TwitchSession, TwitchSessionError>
decode_twitch_session(const support::SecretValue& record) {
    if (record.empty() || record.size() > max_twitch_session_bytes) {
        return std::unexpected(make_error(TwitchSessionErrorCode::ValueTooLarge,
                                          "The saved Twitch session has an invalid size"));
    }

    try {
        Reader reader{record.bytes()};
        const auto magic = reader.read_bytes(session_magic.size());
        if (!magic || !std::ranges::equal(*magic, session_magic)) {
            return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                              "The saved Twitch session is invalid"));
        }

        std::uint32_t version{};
        std::int64_t expires_at{};
        std::uint32_t access_size{};
        std::uint32_t refresh_size{};
        std::uint32_t user_id_size{};
        std::uint32_t login_size{};
        std::uint32_t scope_count{};
        if (!reader.read_u32(version)) {
            return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                              "The saved Twitch session is truncated"));
        }
        if (version != current_twitch_session_version) {
            return std::unexpected(make_error(TwitchSessionErrorCode::UnsupportedVersion,
                                              "The saved Twitch session version is unsupported"));
        }
        if (!reader.read_i64(expires_at) || !reader.read_u32(access_size) ||
            !reader.read_u32(refresh_size) || !reader.read_u32(user_id_size) ||
            !reader.read_u32(login_size) || !reader.read_u32(scope_count) || expires_at <= 0 ||
            expires_at > maximum_unix_seconds || scope_count != 1) {
            return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                              "The saved Twitch session is invalid"));
        }

        const auto access = reader.read_bytes(access_size);
        const auto refresh = reader.read_bytes(refresh_size);
        const auto user_id_bytes = reader.read_bytes(user_id_size);
        const auto login_bytes = reader.read_bytes(login_size);
        std::uint32_t scope_size{};
        if (!access || !refresh || !user_id_bytes || !login_bytes || !reader.read_u32(scope_size)) {
            return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                              "The saved Twitch session is truncated"));
        }
        const auto scope_bytes = reader.read_bytes(scope_size);
        if (!scope_bytes || reader.remaining() != 0 || !valid_token(*access) ||
            !valid_token(*refresh)) {
            return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                              "The saved Twitch session is invalid"));
        }

        auto user_id = text_from_bytes(*user_id_bytes);
        auto login = text_from_bytes(*login_bytes);
        std::vector<std::string> scopes{text_from_bytes(*scope_bytes)};
        if (!valid_user_id(user_id) || !valid_login(login) || !valid_scopes(scopes)) {
            return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                              "The saved Twitch session is invalid"));
        }

        return ports::TwitchSession{
            .credentials =
                ports::TwitchCredentialSet{
                    .access_token = support::SecretValue{std::vector<std::byte>{access->begin(),
                                                                                access->end()}},
                    .refresh_token = support::SecretValue{std::vector<std::byte>{refresh->begin(),
                                                                                 refresh->end()}},
                    .access_expires_at =
                        std::chrono::system_clock::time_point{std::chrono::seconds{expires_at}},
                },
            .user_id = std::move(user_id),
            .login = std::move(login),
            .scopes = std::move(scopes),
        };
    } catch (...) {
        return std::unexpected(make_error(TwitchSessionErrorCode::CorruptRecord,
                                          "The saved Twitch session could not be decoded"));
    }
}

} // namespace manny_uploader::application
