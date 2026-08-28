#include "manny_uploader/ui/donbot_aggregate_selection.hpp"

#include <algorithm>
#include <unordered_set>

namespace manny_uploader::ui {

void DonBotAggregateSelection::reconcile(
    std::string_view identity, std::span<const std::uint64_t> retained_job_ids,
    std::span<const DonBotAggregateCandidate> visible_candidates) {
    if (identity != identity_) {
        identity_ = identity;
        selected_.clear();
        seen_.clear();
    }

    const std::unordered_set<std::uint64_t> retained{retained_job_ids.begin(),
                                                     retained_job_ids.end()};
    std::erase_if(selected_, [&retained](std::uint64_t id) { return !retained.contains(id); });
    std::erase_if(seen_, [&retained](std::uint64_t id) { return !retained.contains(id); });
    for (const auto& candidate : visible_candidates) {
        if (candidate.job_id != 0 && candidate.eligible && retained.contains(candidate.job_id) &&
            seen_.insert(candidate.job_id).second) {
            selected_.insert(candidate.job_id);
        }
    }
}

void DonBotAggregateSelection::set_selected(std::uint64_t job_id, bool selected) {
    if (job_id == 0) {
        return;
    }
    seen_.insert(job_id);
    if (selected) {
        selected_.insert(job_id);
    } else {
        selected_.erase(job_id);
    }
}

bool DonBotAggregateSelection::selected(std::uint64_t job_id) const noexcept {
    return selected_.contains(job_id);
}

std::vector<std::uint64_t> DonBotAggregateSelection::selected_visible(
    std::span<const DonBotAggregateCandidate> visible_candidates) const {
    std::vector<std::uint64_t> result;
    result.reserve(visible_candidates.size());
    for (const auto& candidate : visible_candidates) {
        if (candidate.eligible && selected_.contains(candidate.job_id)) {
            result.push_back(candidate.job_id);
        }
    }
    return result;
}

} // namespace manny_uploader::ui
