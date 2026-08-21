#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace manny_uploader::support {

[[nodiscard]] bool is_valid_utf8(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::size_t>
utf8_code_point_count(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::optional<std::size_t> utf8_code_point_count(std::string_view text) noexcept;

} // namespace manny_uploader::support
