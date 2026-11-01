use std::collections::VecDeque;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::time::Duration;

use super::{
    Clock, ProviderError, UploadIntent, UploadOutcome, UploadProvider, UploadRequest, UploadResult,
    UploadSuccess,
};
use crate::domain::{
    EncounterMetadata, JobError, LogFileIdentity, Provider, ProviderSelection, ProviderState,
    TwitchDeliveryStatus, UploadJob, UploadJobId, UploadJobRecord, WVW_BOSS_ID,
};

pub const MAX_HISTORY_LIMIT: usize = 10_000;
const MAX_RETRY_DELAY: Duration = Duration::from_secs(24 * 60 * 60);
const UPLOADS: [Provider; 3] = [Provider::DpsReport, Provider::Wingman, Provider::DonBot];
const DEPENDENTS: [Provider; 3] = [Provider::Wingman, Provider::DonBot, Provider::Twitch];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CoordinatorError {
    InvalidHistoryLimit,
    DuplicateProvider,
    MissingProvider,
    CapacityReached,
    JobIdExhausted,
    UnknownJob,
    UnexpectedResult,
    InvalidRetryDelay,
    MetadataNotPending,
    RestoreRequiresEmptyCoordinator,
    DependencyUnavailable,
    ShuttingDown,
    Domain(JobError),
    Dispatch(ProviderError),
    Reconfiguration(super::ProviderConfigurationError),
}

impl From<JobError> for CoordinatorError {
    fn from(error: JobError) -> Self {
        Self::Domain(error)
    }
}

impl std::fmt::Display for CoordinatorError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for CoordinatorError {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UploadJobSnapshot {
    pub id: UploadJobId,
    pub record: UploadJobRecord,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DrainSummary {
    pub consumed: usize,
    pub rejected: usize,
    pub provider_errors: usize,
}

struct Entry {
    job: UploadJob,
    metadata_pending: bool,
    intents: [UploadIntent; 4],
    reupload: bool,
}

impl Entry {
    fn new(job: UploadJob, metadata_pending: bool) -> Self {
        Self {
            job,
            metadata_pending,
            intents: [UploadIntent::Automatic; 4],
            reupload: false,
        }
    }

    fn busy(&self) -> bool {
        self.metadata_pending
            || self
                .job
                .snapshot()
                .providers
                .iter()
                .any(|status| status.state.is_busy())
    }
}

/// One application owner mutates jobs. Providers exchange values and UI receives owned snapshots.
pub struct UploadCoordinator<C: Clock> {
    clock: C,
    providers: [Option<Box<dyn UploadProvider>>; 4],
    jobs: VecDeque<Entry>,
    history_limit: usize,
    next_id: Option<u64>,
    restored: bool,
    result_cursor: usize,
    stopping: bool,
}

impl<C: Clock> UploadCoordinator<C> {
    pub fn configure_provider(
        &mut self,
        configuration: super::ProviderConfiguration,
    ) -> Result<(), CoordinatorError> {
        self.require_running()?;
        let provider = self.providers[configuration.policy.provider() as usize]
            .as_mut()
            .ok_or(CoordinatorError::MissingProvider)?;
        catch_unwind(AssertUnwindSafe(|| provider.configure(configuration)))
            .unwrap_or(Err(super::ProviderConfigurationError::Panicked))
            .map_err(CoordinatorError::Reconfiguration)
    }
    pub fn new(
        clock: C,
        providers: Vec<Box<dyn UploadProvider>>,
        history_limit: usize,
    ) -> Result<Self, CoordinatorError> {
        Self::validate_limit(history_limit)?;
        let mut slots: [Option<Box<dyn UploadProvider>>; 4] = std::array::from_fn(|_| None);
        for provider in providers {
            let index = provider.provider() as usize;
            if slots[index].is_some() {
                return Err(CoordinatorError::DuplicateProvider);
            }
            slots[index] = Some(provider);
        }
        Ok(Self {
            clock,
            providers: slots,
            jobs: VecDeque::new(),
            history_limit,
            next_id: Some(1),
            restored: false,
            result_cursor: 0,
            stopping: false,
        })
    }

    pub fn add_pending(
        &mut self,
        file: LogFileIdentity,
        enabled: ProviderSelection,
    ) -> Result<UploadJobId, CoordinatorError> {
        self.require_running()?;
        self.require_providers(
            Provider::ALL
                .into_iter()
                .filter(|provider| enabled.contains(*provider)),
        )?;
        let id = UploadJobId::new(self.next_id.ok_or(CoordinatorError::JobIdExhausted)?)?;
        let job = UploadJob::new(id, file, self.clock.system_now(), enabled)?;
        // Reject before eviction so invalid input or a full busy set cannot erase history.
        if self.jobs.iter().filter(|entry| entry.busy()).count() >= self.history_limit {
            return Err(CoordinatorError::CapacityReached);
        }
        self.trim_to(self.history_limit - 1);
        self.jobs.push_back(Entry::new(job, true));
        self.next_id = id.get().checked_add(1);
        Ok(id)
    }

    pub fn set_metadata(
        &mut self,
        id: UploadJobId,
        metadata: EncounterMetadata,
    ) -> Result<(), CoordinatorError> {
        self.require_running()?;
        let index = self.pending_index(id)?;
        self.jobs[index].job.set_encounter_metadata(metadata)?;
        self.jobs[index].metadata_pending = false;
        if self.state(index, Provider::Wingman) == ProviderState::Waiting && self.is_wvw(index) {
            self.skip_wingman(index)?;
        }
        let result = if self.state(index, Provider::DpsReport) == ProviderState::Waiting {
            self.dispatch(index, Provider::DpsReport)
        } else {
            let wingman = self.dispatch_waiting(index, Provider::Wingman);
            let donbot = self.dispatch_waiting(index, Provider::DonBot);
            wingman.and(donbot)
        };
        self.trim_to(self.history_limit);
        result
    }

    pub fn fail_metadata(
        &mut self,
        id: UploadJobId,
        cancelled: bool,
    ) -> Result<(), CoordinatorError> {
        self.require_running()?;
        let index = self.pending_index(id)?;
        self.jobs[index].metadata_pending = false;
        for provider in Provider::ALL {
            if self.state(index, provider) == ProviderState::Waiting {
                self.jobs[index].job.transition(
                    provider,
                    if cancelled {
                        ProviderState::Cancelled
                    } else {
                        ProviderState::Failed
                    },
                    "Encounter metadata unavailable".into(),
                )?;
            }
        }
        self.trim_to(self.history_limit);
        Ok(())
    }

    pub fn apply_result(&mut self, result: UploadResult) -> Result<(), CoordinatorError> {
        self.require_running()?;
        let index = self.index(result.job_id)?;
        let status = self.jobs[index].job.provider_status(result.provider);
        if status.state != ProviderState::Active || status.attempts != result.attempt {
            return Err(CoordinatorError::UnexpectedResult);
        }
        // Receipt validation and transition form one transaction.
        let mut job = self.jobs[index].job.clone();
        let provider = result.provider;
        let detail = match &result.outcome {
            UploadOutcome::SucceededWithNotice(_, notice) => Some(notice.detail()),
            UploadOutcome::FailedWithNotice(failure) => Some(failure.detail()),
            _ => None,
        };
        let next = match result.outcome {
            UploadOutcome::Succeeded(success) | UploadOutcome::SucceededWithNotice(success, _) => {
                match (provider, success) {
                    (Provider::DpsReport, UploadSuccess::DpsReport(report)) => {
                        job.complete_dps_report(report, detail.unwrap_or("Uploaded").into())?;
                    }
                    (Provider::Wingman, UploadSuccess::Wingman(receipt)) => {
                        if let Some(receipt) = receipt {
                            job.record_wingman_upload(receipt)?;
                        }
                    }
                    (Provider::DonBot, UploadSuccess::DonBot(receipt)) => {
                        if let Some(receipt) = receipt {
                            job.record_donbot_upload(receipt)?;
                        }
                    }
                    (Provider::Twitch, UploadSuccess::Twitch(receipt)) => {
                        job.record_twitch_delivery(receipt)?;
                    }
                    _ => return Err(CoordinatorError::UnexpectedResult),
                }
                ProviderState::Succeeded
            }
            UploadOutcome::Skipped => ProviderState::Skipped,
            UploadOutcome::RetryAfter(delay) => {
                if delay.is_zero() || delay > MAX_RETRY_DELAY {
                    return Err(CoordinatorError::InvalidRetryDelay);
                }
                ProviderState::RetryScheduled(
                    self.clock
                        .steady_now()
                        .checked_add(delay)
                        .ok_or(CoordinatorError::InvalidRetryDelay)?,
                )
            }
            UploadOutcome::Failed
            | UploadOutcome::FailedWithNotice(_)
            | UploadOutcome::Ambiguous => ProviderState::Failed,
            UploadOutcome::TwitchDropped(receipt) => {
                if provider != Provider::Twitch || receipt.status == TwitchDeliveryStatus::Sent {
                    return Err(CoordinatorError::UnexpectedResult);
                }
                job.record_twitch_delivery(receipt)?;
                ProviderState::Failed
            }
            UploadOutcome::Cancelled => ProviderState::Cancelled,
        };
        if !(provider == Provider::DpsReport && next == ProviderState::Succeeded) {
            job.transition(
                provider,
                next,
                detail.unwrap_or(Self::state_detail(next)).into(),
            )?;
        }
        self.jobs[index].job = job;
        if provider == Provider::DpsReport {
            if next == ProviderState::Succeeded {
                for dependent in DEPENDENTS {
                    if dependent != Provider::Twitch || !self.jobs[index].reupload {
                        // A dispatch failure settles its own destination without undoing the report.
                        let _ = self.dispatch_waiting(index, dependent);
                    }
                }
            } else if matches!(
                next,
                ProviderState::Failed | ProviderState::Skipped | ProviderState::Cancelled
            ) {
                self.settle_dependents(index, next == ProviderState::Cancelled);
            }
        }
        self.trim_to(self.history_limit);
        Ok(())
    }

    /// Bounded, round-robin polling prevents a busy destination from starving another.
    pub fn drain_results(&mut self, max_results: usize) -> DrainSummary {
        let mut summary = DrainSummary::default();
        if self.stopping {
            return summary;
        }
        let mut empty = 0;
        while summary.consumed < max_results && empty < Provider::ALL.len() {
            let index = self.result_cursor;
            self.result_cursor = (index + 1) % Provider::ALL.len();
            let Some(provider) = &mut self.providers[index] else {
                empty += 1;
                continue;
            };
            match catch_unwind(AssertUnwindSafe(|| provider.try_take_result()))
                .unwrap_or(Err(ProviderError::Panicked))
            {
                Ok(Some(result)) => {
                    summary.consumed += 1;
                    empty = 0;
                    if result.provider as usize != index || self.apply_result(result).is_err() {
                        summary.rejected += 1;
                    }
                }
                Ok(None) => empty += 1,
                Err(_) => {
                    summary.provider_errors += 1;
                    empty += 1;
                }
            }
        }
        summary
    }

    pub fn dispatch_due_retries(&mut self, max_attempts: usize) -> usize {
        if self.stopping {
            return 0;
        }
        let now = self.clock.steady_now();
        let mut dispatched = 0;
        'jobs: for index in 0..self.jobs.len() {
            for provider in Provider::ALL {
                if dispatched == max_attempts {
                    break 'jobs;
                }
                if matches!(self.state(index, provider), ProviderState::RetryScheduled(deadline) if deadline <= now)
                {
                    let _ = self.dispatch(index, provider);
                    dispatched += 1;
                }
            }
        }
        self.trim_to(self.history_limit);
        dispatched
    }

    pub fn retry_failed(
        &mut self,
        id: UploadJobId,
        provider: Provider,
    ) -> Result<(), CoordinatorError> {
        self.require_running()?;
        let index = self.index(id)?;
        self.require_metadata(index)?;
        self.require_providers([provider])?;
        self.require_dependency(index, provider)?;
        let mut job = self.jobs[index].job.clone();
        if provider == Provider::DpsReport && self.jobs[index].reupload {
            if job.provider_status(provider).state != ProviderState::Failed {
                return Err(JobError::ManualRetryRequiresFailure.into());
            }
            job.transition(provider, ProviderState::Waiting, String::new())?;
            for dependent in [Provider::Wingman, Provider::DonBot] {
                if job.provider_status(dependent).state == ProviderState::Skipped {
                    job.prepare_explicit_delivery(dependent)?;
                }
            }
        } else {
            job.prepare_manual_retry(provider)?;
        }
        self.jobs[index].job = job;
        if self.jobs[index].intents[provider as usize] != UploadIntent::Reupload {
            self.jobs[index].intents[provider as usize] = UploadIntent::ManualRetry;
        }
        self.dispatch(index, provider)
    }

    pub fn reupload(&mut self, id: UploadJobId) -> Result<(), CoordinatorError> {
        self.require_running()?;
        self.require_providers(UPLOADS)?;
        let index = self.index(id)?;
        let mut job = self.jobs[index].job.clone();
        for provider in UPLOADS {
            job.prepare_explicit_delivery(provider)?;
        }
        self.jobs[index].job = job;
        self.jobs[index].reupload = true;
        for provider in UPLOADS {
            self.jobs[index].intents[provider as usize] = UploadIntent::Reupload;
        }
        if self.is_wvw(index) {
            self.skip_wingman(index)?;
        }
        self.dispatch(index, Provider::DpsReport)
    }

    pub fn rechat(&mut self, id: UploadJobId) -> Result<(), CoordinatorError> {
        self.require_running()?;
        self.require_providers([Provider::Twitch])?;
        let index = self.index(id)?;
        self.require_dependency(index, Provider::Twitch)?;
        self.jobs[index]
            .job
            .prepare_explicit_delivery(Provider::Twitch)?;
        self.jobs[index].intents[Provider::Twitch as usize] = UploadIntent::Rechat;
        self.dispatch(index, Provider::Twitch)
    }

    /// Restore validates the retained tail before committing anything and never dispatches work.
    pub fn restore(&mut self, records: Vec<UploadJobRecord>) -> Result<(), CoordinatorError> {
        self.require_running()?;
        if self.restored || self.next_id != Some(1) || !self.jobs.is_empty() {
            return Err(CoordinatorError::RestoreRequiresEmptyCoordinator);
        }
        let skip = records.len().saturating_sub(self.history_limit);
        let mut jobs = VecDeque::new();
        for (index, record) in records.into_iter().skip(skip).enumerate() {
            let id = UploadJobId::new(index as u64 + 1)?;
            jobs.push_back(Entry::new(UploadJob::restore(id, record)?, false));
        }
        self.next_id = Some(jobs.len() as u64 + 1);
        self.jobs = jobs;
        self.restored = true;
        Ok(())
    }

    pub fn snapshots(&self) -> Vec<UploadJobSnapshot> {
        self.jobs
            .iter()
            .map(|entry| UploadJobSnapshot {
                id: entry.job.id(),
                record: entry.job.snapshot().clone(),
            })
            .collect()
    }

    pub fn snapshot(&self, id: UploadJobId) -> Result<UploadJobSnapshot, CoordinatorError> {
        let entry = &self.jobs[self.index(id)?];
        Ok(UploadJobSnapshot {
            id,
            record: entry.job.snapshot().clone(),
        })
    }

    pub fn history(&self) -> Vec<UploadJobRecord> {
        self.jobs
            .iter()
            .map(|entry| entry.job.snapshot().clone())
            .collect()
    }

    pub fn set_history_limit(&mut self, limit: usize) -> Result<(), CoordinatorError> {
        Self::validate_limit(limit)?;
        self.history_limit = limit;
        self.trim_to(limit);
        Ok(())
    }

    pub fn cancel_all(&mut self) {
        if self.stopping {
            return;
        }
        self.stopping = true;
        for provider in self.providers.iter_mut().flatten() {
            let _ = catch_unwind(AssertUnwindSafe(|| provider.cancel_pending()));
        }
        for entry in &mut self.jobs {
            entry.metadata_pending = false;
            for provider in Provider::ALL {
                if entry.job.provider_status(provider).state.is_busy() {
                    let _ = entry.job.transition(
                        provider,
                        ProviderState::Cancelled,
                        "Cancelled".into(),
                    );
                }
            }
        }
        self.trim_to(self.history_limit);
    }

    fn dispatch_waiting(
        &mut self,
        index: usize,
        provider: Provider,
    ) -> Result<(), CoordinatorError> {
        if self.state(index, provider) == ProviderState::Waiting {
            self.dispatch(index, provider)
        } else {
            Ok(())
        }
    }

    fn dispatch(&mut self, index: usize, provider: Provider) -> Result<(), CoordinatorError> {
        if provider == Provider::Wingman && self.is_wvw(index) {
            return self.skip_wingman(index);
        }
        let admitted = self
            .require_metadata(index)
            .and_then(|()| self.require_providers([provider]))
            .and_then(|()| self.require_dependency(index, provider))
            .and_then(|()| {
                self.jobs[index]
                    .job
                    .transition(provider, ProviderState::Active, "Uploading".into())
                    .map_err(Into::into)
            });
        if let Err(error) = admitted {
            self.fail_dispatch(index, provider);
            return Err(error);
        }
        let entry = &self.jobs[index];
        let record = entry.job.snapshot();
        let request = UploadRequest {
            job_id: entry.job.id(),
            provider,
            attempt: entry.job.provider_status(provider).attempts,
            file: record.file.clone(),
            metadata: record
                .encounter_metadata
                .clone()
                .expect("validated metadata"),
            dps_report_result: if provider == Provider::DpsReport {
                None
            } else {
                record.dps_report_result.clone()
            },
            intent: entry.intents[provider as usize],
        };
        let adapter = self.providers[provider as usize]
            .as_mut()
            .expect("validated provider");
        let result = catch_unwind(AssertUnwindSafe(|| adapter.enqueue(request)))
            .unwrap_or(Err(ProviderError::Panicked));
        if let Err(error) = result {
            self.fail_dispatch(index, provider);
            return Err(CoordinatorError::Dispatch(error));
        }
        Ok(())
    }

    fn fail_dispatch(&mut self, index: usize, provider: Provider) {
        let _ = self.jobs[index].job.transition(
            provider,
            ProviderState::Failed,
            "Provider unavailable".into(),
        );
        if provider == Provider::DpsReport {
            self.settle_dependents(index, false);
        }
    }

    fn settle_dependents(&mut self, index: usize, cancelled: bool) {
        for provider in DEPENDENTS {
            if provider == Provider::Twitch && self.jobs[index].reupload {
                continue;
            }
            if self.state(index, provider) == ProviderState::Waiting {
                let _ = self.jobs[index].job.transition(
                    provider,
                    if cancelled {
                        ProviderState::Cancelled
                    } else {
                        ProviderState::Skipped
                    },
                    "dps.report result unavailable".into(),
                );
            }
        }
    }

    fn require_dependency(&self, index: usize, provider: Provider) -> Result<(), CoordinatorError> {
        if provider == Provider::DpsReport {
            return Ok(());
        }
        let state = self.state(index, Provider::DpsReport);
        if provider != Provider::Twitch && state == ProviderState::Disabled {
            return Ok(());
        }
        if state == ProviderState::Succeeded
            && self.jobs[index].job.snapshot().dps_report_result.is_some()
        {
            Ok(())
        } else {
            Err(CoordinatorError::DependencyUnavailable)
        }
    }

    fn pending_index(&self, id: UploadJobId) -> Result<usize, CoordinatorError> {
        let index = self.index(id)?;
        let record = self.jobs[index].job.snapshot();
        if !self.jobs[index].metadata_pending
            || record.encounter_metadata.is_some()
            || record.providers.iter().any(|status| {
                !matches!(
                    status.state,
                    ProviderState::Waiting | ProviderState::Disabled
                )
            })
        {
            return Err(CoordinatorError::MetadataNotPending);
        }
        Ok(index)
    }

    fn require_metadata(&self, index: usize) -> Result<(), CoordinatorError> {
        if self.jobs[index].job.snapshot().encounter_metadata.is_none() {
            Err(JobError::EncounterMetadataRequired.into())
        } else {
            Ok(())
        }
    }

    fn index(&self, id: UploadJobId) -> Result<usize, CoordinatorError> {
        self.jobs
            .iter()
            .position(|entry| entry.job.id() == id)
            .ok_or(CoordinatorError::UnknownJob)
    }

    fn state(&self, index: usize, provider: Provider) -> ProviderState {
        self.jobs[index].job.provider_status(provider).state
    }

    fn is_wvw(&self, index: usize) -> bool {
        self.jobs[index]
            .job
            .snapshot()
            .encounter_metadata
            .as_ref()
            .is_some_and(|metadata| metadata.boss_id == WVW_BOSS_ID)
    }

    fn skip_wingman(&mut self, index: usize) -> Result<(), CoordinatorError> {
        self.jobs[index].job.transition(
            Provider::Wingman,
            ProviderState::Skipped,
            "WvW is unsupported by GW2Wingman".into(),
        )?;
        Ok(())
    }

    fn require_running(&self) -> Result<(), CoordinatorError> {
        if self.stopping {
            Err(CoordinatorError::ShuttingDown)
        } else {
            Ok(())
        }
    }

    fn require_providers(
        &self,
        providers: impl IntoIterator<Item = Provider>,
    ) -> Result<(), CoordinatorError> {
        if providers
            .into_iter()
            .any(|provider| self.providers[provider as usize].is_none())
        {
            Err(CoordinatorError::MissingProvider)
        } else {
            Ok(())
        }
    }

    fn validate_limit(limit: usize) -> Result<(), CoordinatorError> {
        if (1..=MAX_HISTORY_LIMIT).contains(&limit) {
            Ok(())
        } else {
            Err(CoordinatorError::InvalidHistoryLimit)
        }
    }

    fn trim_to(&mut self, limit: usize) {
        while self.jobs.len() > limit {
            let Some(index) = self.jobs.iter().position(|entry| !entry.busy()) else {
                break;
            };
            self.jobs.remove(index);
        }
    }

    fn state_detail(state: ProviderState) -> &'static str {
        match state {
            ProviderState::Succeeded => "Completed",
            ProviderState::Skipped => "Skipped",
            ProviderState::RetryScheduled(_) => "Retry scheduled",
            ProviderState::Cancelled => "Cancelled",
            _ => "Delivery failed",
        }
    }
}

impl<C: Clock> Drop for UploadCoordinator<C> {
    fn drop(&mut self) {
        self.cancel_all();
    }
}
