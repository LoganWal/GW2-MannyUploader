use std::collections::HashSet;
use std::panic::{AssertUnwindSafe, catch_unwind};

use super::{
    Clock, CoordinatorError, LogMetadataParser, MetadataError, MetadataRequest, MetadataResult,
    ParserError, UploadCoordinator,
};
use crate::domain::{LogFileIdentity, ProviderSelection, UploadJobId};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IngestionError {
    ShuttingDown,
    ParserFull,
    UnexpectedResult,
    ResourceLimit,
    Parser(ParserError),
    Coordinator(CoordinatorError),
}

impl From<CoordinatorError> for IngestionError {
    fn from(error: CoordinatorError) -> Self {
        Self::Coordinator(error)
    }
}

impl std::fmt::Display for IngestionError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for IngestionError {}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MetadataDrainSummary {
    pub consumed: usize,
    pub rejected: usize,
    pub parser_errors: usize,
}

/// The application owner admits metadata work here and consumes results before scanning new files.
pub struct LogIngestionCoordinator<C: Clock> {
    uploads: UploadCoordinator<C>,
    parser: Box<dyn LogMetadataParser>,
    pending: HashSet<UploadJobId>,
    stopping: bool,
}

impl<C: Clock> LogIngestionCoordinator<C> {
    pub fn new(uploads: UploadCoordinator<C>, parser: Box<dyn LogMetadataParser>) -> Self {
        Self {
            uploads,
            parser,
            pending: HashSet::new(),
            stopping: false,
        }
    }

    pub fn available_capacity(&self) -> Result<usize, IngestionError> {
        if self.stopping {
            return Ok(0);
        }
        catch_unwind(AssertUnwindSafe(|| self.parser.available_capacity()))
            .map_err(|_| IngestionError::Parser(ParserError::Panicked))
    }

    pub fn submit(
        &mut self,
        file: LogFileIdentity,
        enabled: ProviderSelection,
    ) -> Result<UploadJobId, IngestionError> {
        if self.stopping {
            return Err(IngestionError::ShuttingDown);
        }
        if self.available_capacity()? == 0 {
            return Err(IngestionError::ParserFull);
        }
        self.pending
            .try_reserve(1)
            .map_err(|_| IngestionError::ResourceLimit)?;
        let id = self.uploads.add_pending(file.clone(), enabled)?;
        let admitted = catch_unwind(AssertUnwindSafe(|| {
            self.parser.enqueue(MetadataRequest { job_id: id, file })
        }))
        .unwrap_or(Err(ParserError::Panicked));
        if admitted.is_ok() {
            self.pending.insert(id);
        } else {
            // A post-capacity-check rejection retains a visible accepted job for deduplication.
            self.uploads.fail_metadata(id, false)?;
        }
        Ok(id)
    }

    pub fn handle_result(&mut self, result: MetadataResult) -> Result<(), IngestionError> {
        if self.stopping {
            return Err(IngestionError::ShuttingDown);
        }
        if !self.pending.remove(&result.job_id) {
            return Err(IngestionError::UnexpectedResult);
        }
        match result.metadata {
            Ok(metadata) => self.uploads.set_metadata(result.job_id, metadata)?,
            Err(error) => self
                .uploads
                .fail_metadata(result.job_id, error == MetadataError::Cancelled)?,
        }
        Ok(())
    }

    pub fn drain_metadata(&mut self, max_results: usize) -> MetadataDrainSummary {
        let mut summary = MetadataDrainSummary::default();
        if self.stopping {
            return summary;
        }
        for _ in 0..max_results {
            match catch_unwind(AssertUnwindSafe(|| self.parser.try_take_result()))
                .unwrap_or(Err(ParserError::Panicked))
            {
                Ok(Some(result)) => {
                    summary.consumed += 1;
                    if self.handle_result(result).is_err() {
                        summary.rejected += 1;
                    }
                }
                Ok(None) => break,
                Err(_) => {
                    summary.parser_errors += 1;
                    break;
                }
            }
        }
        summary
    }

    pub fn uploads(&self) -> &UploadCoordinator<C> {
        &self.uploads
    }

    /// Application commands use this for upload polling and explicit replay. Metadata admission
    /// must continue through submit so results remain correlated with this owner's pending set.
    pub fn uploads_mut(&mut self) -> &mut UploadCoordinator<C> {
        &mut self.uploads
    }

    pub fn cancel_all(&mut self) {
        if self.stopping {
            return;
        }
        self.stopping = true;
        let _ = catch_unwind(AssertUnwindSafe(|| self.parser.cancel_pending()));
        self.uploads.cancel_all();
        self.pending.clear();
    }
}

impl<C: Clock> Drop for LogIngestionCoordinator<C> {
    fn drop(&mut self) {
        self.cancel_all();
    }
}
