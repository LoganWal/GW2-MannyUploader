#include "manny_uploader/application/log_selection.hpp"
#include "support/test_suite.hpp"

#include <chrono>
#include <filesystem>

namespace manny_uploader::test {

void run_log_selection_tests(TestSuite& suite) {
    const auto system_anchor = std::chrono::system_clock::time_point{} + std::chrono::hours{50};
    const auto file_anchor = std::filesystem::file_time_type{} + std::chrono::hours{7};
    MANNY_CHECK(suite, application::file_time_from_system(system_anchor + std::chrono::hours{3},
                                                          system_anchor, file_anchor) ==
                           file_anchor + std::chrono::hours{3});
    MANNY_CHECK(suite, application::file_time_from_system(system_anchor - std::chrono::minutes{15},
                                                          system_anchor, file_anchor) ==
                           file_anchor - std::chrono::minutes{15});

    const auto session_started_at = std::filesystem::file_time_type{} + std::chrono::hours{18};
    const auto last_24_hours_started_at = std::filesystem::file_time_type{};
    const auto window = application::LogSelectionWindow{
        .session_started_at = session_started_at,
        .last_24_hours_started_at = last_24_hours_started_at,
    };

    MANNY_CHECK(suite, application::log_selection_cutoff(application::LogSelectionMode::New,
                                                         window) == session_started_at);
    MANNY_CHECK(suite, application::log_selection_cutoff(application::LogSelectionMode::Last24Hours,
                                                         window) == last_24_hours_started_at);
    MANNY_CHECK(suite,
                !application::log_matches_selection(session_started_at - std::chrono::seconds{1},
                                                    application::LogSelectionMode::New, window));
    MANNY_CHECK(suite, application::log_matches_selection(
                           session_started_at, application::LogSelectionMode::New, window));
    MANNY_CHECK(suite, application::log_matches_selection(
                           session_started_at - std::chrono::hours{1},
                           application::LogSelectionMode::Last24Hours, window));
    MANNY_CHECK(suite, !application::log_matches_selection(
                           last_24_hours_started_at - std::chrono::seconds{1},
                           application::LogSelectionMode::Last24Hours, window));
}

} // namespace manny_uploader::test
