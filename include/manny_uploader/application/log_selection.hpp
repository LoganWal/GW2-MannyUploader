#pragma once

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
