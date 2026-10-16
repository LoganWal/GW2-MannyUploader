use std::panic::{AssertUnwindSafe, catch_unwind};

use super::{
    CandidateError, CandidateIssue, CandidateSourceOptions, Clock, DrainSummary, IngestionError,
    LogCandidateSource, LogIngestionCoordinator, MetadataDrainSummary, StopToken,
};
use crate::domain::{DiscoveryError, LogDiscovery, LogFileIdentity, ProviderSelection};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PumpError {
    InvalidConfiguration,
    ShuttingDown,
    Source(CandidateError),
    Discovery(DiscoveryError),
    Ingestion(IngestionError),
}

#[derive(Clone, Copy, Debug)]
pub struct PumpOptions {
    pub required_matches: usize,
    pub pending_limit: usize,
    pub history_limit: usize,
    pub max_metadata_results: usize,
    pub max_upload_results: usize,
    pub max_retries: usize,
}

impl Default for PumpOptions {
    fn default() -> Self {
        Self {
            required_matches: 2,
            pending_limit: 10_000,
            history_limit: 10_000,
            max_metadata_results: 8,
            max_upload_results: 8,
            max_retries: 8,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TickReport {
    pub metadata: MetadataDrainSummary,
    pub uploads: DrainSummary,
    pub retries: usize,
    pub root_available: bool,
    pub scan_complete: bool,
    pub observations: usize,
    pub removed: usize,
    pub submitted: usize,
    pub issues: Vec<CandidateIssue>,
}

/// Owned by the application thread. All I/O and mutable job access stay off render callbacks.
pub struct ApplicationPump<C: Clock> {
    source: Box<dyn LogCandidateSource>,
    ingestion: LogIngestionCoordinator<C>,
    discovery: LogDiscovery,
    options: PumpOptions,
    stopping: bool,
}

impl<C: Clock> ApplicationPump<C> {
    pub fn new(
        source: Box<dyn LogCandidateSource>,
        ingestion: LogIngestionCoordinator<C>,
        options: PumpOptions,
    ) -> Result<Self, PumpError> {
        if [
            options.max_metadata_results,
            options.max_upload_results,
            options.max_retries,
        ]
        .iter()
        .any(|limit| !(1..=10_000).contains(limit))
        {
            return Err(PumpError::InvalidConfiguration);
        }
        let discovery = LogDiscovery::new(
            options.required_matches,
            options.pending_limit,
            options.history_limit,
        )
        .map_err(PumpError::Discovery)?;
        Ok(Self {
            source,
            ingestion,
            discovery,
            options,
            stopping: false,
        })
    }

    pub fn tick(
        &mut self,
        enabled: ProviderSelection,
        stop: &StopToken,
    ) -> Result<TickReport, PumpError> {
        if self.stopping {
            return Err(PumpError::ShuttingDown);
        }
        if stop.is_stop_requested() {
            return Err(PumpError::Source(CandidateError::Cancelled));
        }
        let metadata = self
            .ingestion
            .drain_metadata(self.options.max_metadata_results);
        let uploads = self
            .ingestion
            .uploads_mut()
            .drain_results(self.options.max_upload_results);
        let retries = self
            .ingestion
            .uploads_mut()
            .dispatch_due_retries(self.options.max_retries);
        let batch = catch_unwind(AssertUnwindSafe(|| self.source.poll(stop)))
            .unwrap_or(Err(CandidateError::Panicked))
            .map_err(PumpError::Source)?;
        for path in &batch.removed_paths {
            self.discovery.forget_pending(path);
        }
        let mut report = TickReport {
            metadata,
            uploads,
            retries,
            root_available: batch.root_available,
            scan_complete: batch.scan_complete,
            observations: batch.observations.len(),
            removed: batch.removed_paths.len(),
            issues: batch.issues,
            ..Default::default()
        };
        for observation in batch.observations {
            if stop.is_stop_requested() {
                return Err(PumpError::Source(CandidateError::Cancelled));
            }
            if self
                .ingestion
                .available_capacity()
                .map_err(PumpError::Ingestion)?
                == 0
            {
                break;
            }
            if let Some(file) = self
                .discovery
                .observe(observation)
                .map_err(PumpError::Discovery)?
            {
                if let Err(error) = self.ingestion.submit(file.clone(), enabled) {
                    self.discovery.release(&file);
                    return Err(PumpError::Ingestion(error));
                }
                report.submitted += 1;
            }
        }
        Ok(report)
    }

    pub fn reconfigure_source(&mut self, options: CandidateSourceOptions) -> Result<(), PumpError> {
        if self.stopping {
            return Err(PumpError::ShuttingDown);
        }
        catch_unwind(AssertUnwindSafe(|| self.source.reconfigure(options)))
            .unwrap_or(Err(CandidateError::Panicked))
            .map_err(PumpError::Source)?;
        self.discovery.clear_pending();
        Ok(())
    }

    pub fn set_required_matches(&mut self, matches: usize) -> Result<(), PumpError> {
        if self.stopping {
            return Err(PumpError::ShuttingDown);
        }
        self.discovery
            .set_required_matches(matches)
            .map_err(PumpError::Discovery)
    }

    pub fn seed_processed_logs(&mut self, files: &[LogFileIdentity]) -> Result<(), PumpError> {
        if self.stopping {
            return Err(PumpError::ShuttingDown);
        }
        self.discovery.seed(files).map_err(PumpError::Discovery)
    }

    pub fn ingestion(&self) -> &LogIngestionCoordinator<C> {
        &self.ingestion
    }

    /// For application commands and snapshots. Only tick should admit discovered files.
    pub fn ingestion_mut(&mut self) -> &mut LogIngestionCoordinator<C> {
        &mut self.ingestion
    }

    pub fn cancel_all(&mut self) {
        if !self.stopping {
            self.stopping = true;
            self.discovery.clear_pending();
            self.ingestion.cancel_all();
        }
    }
}

impl<C: Clock> Drop for ApplicationPump<C> {
    fn drop(&mut self) {
        self.cancel_all();
    }
}
