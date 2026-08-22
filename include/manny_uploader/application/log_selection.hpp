#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace manny_uploader::application {

enum class LogSelectionMode : std::uint8_t {
    New,
    Today,
};

struct LogSelectionWindow {
    std::filesystem::file_time_type session_started_at;
    std::filesystem::file_time_type local_day_started_at;
};

[[nodiscard]] constexpr std::filesystem::file_time_type
file_time_from_system(std::chrono::system_clock::time_point value,
                      std::chrono::system_clock::time_point system_anchor,
                      std::filesystem::file_time_type file_anchor) noexcept {
    return file_anchor + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                             value - system_anchor);
}

[[nodiscard]] inline std::filesystem::file_time_type
file_time_from_system(std::chrono::system_clock::time_point value) noexcept {
    const auto system_anchor = std::chrono::system_clock::now();
    const auto file_anchor = std::filesystem::file_time_type::clock::now();
    return file_time_from_system(value, system_anchor, file_anchor);
}

[[nodiscard]] constexpr std::filesystem::file_time_type
log_selection_cutoff(LogSelectionMode mode, const LogSelectionWindow& window) noexcept {
    return mode == LogSelectionMode::Today ? window.local_day_started_at
                                           : window.session_started_at;
}

[[nodiscard]] constexpr bool log_matches_selection(std::filesystem::file_time_type last_write_time,
                                                   LogSelectionMode mode,
                                                   const LogSelectionWindow& window) noexcept {
    return last_write_time >= log_selection_cutoff(mode, window);
}

} // namespace manny_uploader::application
