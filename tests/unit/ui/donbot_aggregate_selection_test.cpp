#include "manny_uploader/ui/donbot_aggregate_selection.hpp"

#include "support/test_suite.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace manny_uploader::test {

void run_donbot_aggregate_selection_tests(TestSuite& suite) {
    ui::DonBotAggregateSelection selection;
    constexpr std::array<std::uint64_t, 3> retained{1, 2, 3};
    constexpr std::array candidates{
        ui::DonBotAggregateCandidate{.job_id = 1, .eligible = true},
        ui::DonBotAggregateCandidate{.job_id = 2, .eligible = false},
    };
    selection.reconcile("server/account/guild", retained, candidates);
    MANNY_CHECK(suite, selection.selected(1));
    MANNY_CHECK(suite, !selection.selected(2));

    selection.set_selected(1, false);
    selection.reconcile("server/account/guild", retained, candidates);
    MANNY_CHECK(suite, !selection.selected(1));

    constexpr std::array temporarily_unavailable{
        ui::DonBotAggregateCandidate{.job_id = 1, .eligible = false},
        ui::DonBotAggregateCandidate{.job_id = 2, .eligible = false},
    };
    selection.reconcile("server/account/guild", retained, temporarily_unavailable);
    selection.reconcile("server/account/guild", retained, candidates);
    MANNY_CHECK(suite, !selection.selected(1));

    constexpr std::array newly_eligible{
        ui::DonBotAggregateCandidate{.job_id = 1, .eligible = true},
        ui::DonBotAggregateCandidate{.job_id = 2, .eligible = true},
        ui::DonBotAggregateCandidate{.job_id = 3, .eligible = true},
    };
    selection.reconcile("server/account/guild", retained, newly_eligible);
    MANNY_CHECK(suite, !selection.selected(1));
    MANNY_CHECK(suite, selection.selected(2));
    MANNY_CHECK(suite, selection.selected(3));
    MANNY_CHECK(suite,
                selection.selected_visible(newly_eligible) == std::vector<std::uint64_t>({2, 3}));

    constexpr std::array second_ineligible{
        ui::DonBotAggregateCandidate{.job_id = 1, .eligible = true},
        ui::DonBotAggregateCandidate{.job_id = 2, .eligible = false},
        ui::DonBotAggregateCandidate{.job_id = 3, .eligible = true},
    };
    selection.reconcile("server/account/guild", retained, second_ineligible);
    MANNY_CHECK(suite, selection.selected(2));
    MANNY_CHECK(suite,
                selection.selected_visible(second_ineligible) == std::vector<std::uint64_t>({3}));
    selection.reconcile("server/account/guild", retained, newly_eligible);
    MANNY_CHECK(suite, selection.selected(2));

    constexpr std::array<std::uint64_t, 2> pruned_retained{1, 2};
    selection.reconcile("server/account/guild", pruned_retained, newly_eligible);
    MANNY_CHECK(suite, !selection.selected(3));

    selection.reconcile("server/account/other-guild", pruned_retained, newly_eligible);
    MANNY_CHECK(suite, selection.selected(1));
    MANNY_CHECK(suite, selection.selected(2));
}

} // namespace manny_uploader::test
