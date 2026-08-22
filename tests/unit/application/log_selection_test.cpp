#include "manny_uploader/application/log_selection.hpp"
#include "support/test_suite.hpp"

#include <chrono>
#include <filesystem>

namespace manny_uploader::test {

void run_log_selection_tests(TestSuite& suite) {
    const auto session_started_at = std::filesystem::file_time_type{} + std::chrono::hours{18};
    const auto local_day_started_at = std::filesystem::file_time_type{};
    const auto window = application::LogSelectionWindow{
        .session_started_at = session_started_at,
        .local_day_started_at = local_day_started_at,
    };

    MANNY_CHECK(suite, application::log_selection_cutoff(application::LogSelectionMode::New,
                                                         window) == session_started_at);
    MANNY_CHECK(suite, application::log_selection_cutoff(application::LogSelectionMode::Today,
                                                         window) == local_day_started_at);
    MANNY_CHECK(suite,
                !application::log_matches_selection(session_started_at - std::chrono::seconds{1},
                                                    application::LogSelectionMode::New, window));
    MANNY_CHECK(suite, application::log_matches_selection(
                           session_started_at, application::LogSelectionMode::New, window));
    MANNY_CHECK(suite,
                application::log_matches_selection(session_started_at - std::chrono::hours{1},
                                                   application::LogSelectionMode::Today, window));
    MANNY_CHECK(suite,
                !application::log_matches_selection(local_day_started_at - std::chrono::seconds{1},
                                                    application::LogSelectionMode::Today, window));
}

} // namespace manny_uploader::test
