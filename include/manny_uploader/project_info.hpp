#pragma once

#include <cstdint>
#include <string_view>

namespace manny_uploader {

struct Version {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};

struct ProjectInfo {
    std::string_view name;
    std::string_view version_text;
    Version version;
};

[[nodiscard]] const ProjectInfo& project_info() noexcept;

} // namespace manny_uploader
