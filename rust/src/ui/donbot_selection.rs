use crate::domain::UploadJobId;
use std::collections::HashSet;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotSelectionIdentity {
    pub api_base_url: String,
    pub account_name: String,
    pub guild_id: String,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DonBotAggregateCandidate {
    pub job_id: UploadJobId,
    pub eligible: bool,
}
#[derive(Default)]
pub struct DonBotAggregateSelection {
    identity: Option<DonBotSelectionIdentity>,
    retained: HashSet<UploadJobId>,
    seen: HashSet<UploadJobId>,
    selected: HashSet<UploadJobId>,
}
impl DonBotAggregateSelection {
    pub fn reconcile(
        &mut self,
        identity: Option<DonBotSelectionIdentity>,
        retained: &[UploadJobId],
        visible: &[DonBotAggregateCandidate],
    ) {
        if self.identity != identity {
            self.seen.clear();
            self.selected.clear();
            self.identity = identity;
        }
        self.retained = retained.iter().copied().collect();
        self.seen.retain(|id| self.retained.contains(id));
        self.selected.retain(|id| self.retained.contains(id));
        for candidate in visible {
            if candidate.eligible
                && self.retained.contains(&candidate.job_id)
                && self.seen.insert(candidate.job_id)
            {
                self.selected.insert(candidate.job_id);
            }
        }
    }
    pub fn set_selected(&mut self, job_id: UploadJobId, selected: bool) {
        if !self.retained.contains(&job_id) {
            return;
        }
        self.seen.insert(job_id);
        if selected {
            self.selected.insert(job_id);
        } else {
            self.selected.remove(&job_id);
        }
    }
    pub fn selected(&self, job_id: UploadJobId) -> bool {
        self.selected.contains(&job_id)
    }
    pub fn selected_visible(&self, visible: &[DonBotAggregateCandidate]) -> Vec<UploadJobId> {
        let mut included = HashSet::new();
        visible
            .iter()
            .filter(|candidate| {
                candidate.eligible
                    && self.selected.contains(&candidate.job_id)
                    && included.insert(candidate.job_id)
            })
            .map(|candidate| candidate.job_id)
            .collect()
    }
}
