#pragma once

#include "manny_uploader/ports/twitch_authenticator.hpp"
#include "manny_uploader/support/secret_value.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace manny_uploader::application {

inline constexpr std::uint32_t current_twitch_session_version = 1;
inline constexpr std::size_t max_twitch_session_bytes = std::size_t{16} * 1024U;

enum class TwitchSessionErrorCode : std::uint8_t {
    InvalidSession,
    ValueTooLarge,
    UnsupportedVersion,
    CorruptRecord,
};

struct TwitchSessionError {
    TwitchSessionErrorCode code;
    std::string message;
};

[[nodiscard]] std::expected<support::SecretValue, TwitchSessionError>
encode_twitch_session(const ports::TwitchSession& session);

[[nodiscard]] std::expected<ports::TwitchSession, TwitchSessionError>
decode_twitch_session(const support::SecretValue& record);

} // namespace manny_uploader::application
