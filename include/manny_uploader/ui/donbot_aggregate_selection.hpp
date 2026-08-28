#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace manny_uploader::ui {

struct DonBotAggregateCandidate {
    std::uint64_t job_id{};
    bool eligible{};
};

class DonBotAggregateSelection {
  public:
    void reconcile(std::string_view identity, std::span<const std::uint64_t> retained_job_ids,
                   std::span<const DonBotAggregateCandidate> visible_candidates);
    void set_selected(std::uint64_t job_id, bool selected);
    [[nodiscard]] bool selected(std::uint64_t job_id) const noexcept;
    [[nodiscard]] std::vector<std::uint64_t>
    selected_visible(std::span<const DonBotAggregateCandidate> visible_candidates) const;

  private:
    std::unordered_set<std::uint64_t> selected_;
    std::unordered_set<std::uint64_t> seen_;
    std::string identity_;
};

} // namespace manny_uploader::ui
