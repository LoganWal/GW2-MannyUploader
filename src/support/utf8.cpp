#include "manny_uploader/support/utf8.hpp"

#include <cstdint>
#include <optional>

namespace manny_uploader::support {
namespace {

[[nodiscard]] bool is_continuation(std::uint8_t byte) noexcept {
    return byte >= 0x80U && byte <= 0xbfU;
}

[[nodiscard]] std::uint8_t byte_at(std::span<const std::byte> text, std::size_t index) noexcept {
    return std::to_integer<std::uint8_t>(text[index]);
}

[[nodiscard]] bool valid_three_byte_sequence(std::uint8_t lead, std::uint8_t second,
                                             std::uint8_t third) noexcept {
    const bool valid_second =
        (lead == 0xe0U && second >= 0xa0U && second <= 0xbfU) ||
        (lead == 0xedU && second >= 0x80U && second <= 0x9fU) ||
        (((lead >= 0xe1U && lead <= 0xecU) || (lead >= 0xeeU && lead <= 0xefU)) &&
         is_continuation(second));
    return valid_second && is_continuation(third);
}

[[nodiscard]] bool valid_four_byte_sequence(std::uint8_t lead, std::uint8_t second,
                                            std::uint8_t third, std::uint8_t fourth) noexcept {
    const bool valid_second = (lead == 0xf0U && second >= 0x90U && second <= 0xbfU) ||
                              (lead == 0xf4U && second >= 0x80U && second <= 0x8fU) ||
                              (lead >= 0xf1U && lead <= 0xf3U && is_continuation(second));
    return valid_second && is_continuation(third) && is_continuation(fourth);
}

[[nodiscard]] std::optional<std::size_t> sequence_width(std::span<const std::byte> text,
                                                        std::size_t index) noexcept {
    const auto lead = byte_at(text, index);
    const auto remaining = text.size() - index;
    if (lead <= 0x7fU) {
        return 1;
    }
    if (lead >= 0xc2U && lead <= 0xdfU && remaining >= 2 &&
        is_continuation(byte_at(text, index + 1))) {
        return 2;
    }
    if (lead >= 0xe0U && lead <= 0xefU && remaining >= 3 &&
        valid_three_byte_sequence(lead, byte_at(text, index + 1), byte_at(text, index + 2))) {
        return 3;
    }
    if (lead >= 0xf0U && lead <= 0xf4U && remaining >= 4 &&
        valid_four_byte_sequence(lead, byte_at(text, index + 1), byte_at(text, index + 2),
                                 byte_at(text, index + 3))) {
        return 4;
    }
    return std::nullopt;
}

} // namespace

bool is_valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t index{};
    while (index < bytes.size()) {
        const auto width = sequence_width(bytes, index);
        if (!width.has_value()) {
            return false;
        }
        index += *width;
    }
    return true;
}

bool is_valid_utf8(std::string_view text) noexcept {
    return is_valid_utf8(std::as_bytes(std::span{text.data(), text.size()}));
}

std::optional<std::size_t> utf8_code_point_count(std::span<const std::byte> bytes) noexcept {
    std::size_t index{};
    std::size_t count{};
    while (index < bytes.size()) {
        const auto width = sequence_width(bytes, index);
        if (!width.has_value()) {
            return std::nullopt;
        }
        index += *width;
        ++count;
    }
    return count;
}

std::optional<std::size_t> utf8_code_point_count(std::string_view text) noexcept {
    return utf8_code_point_count(std::as_bytes(std::span{text.data(), text.size()}));
}

} // namespace manny_uploader::support
