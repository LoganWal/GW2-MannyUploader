use std::time::Duration;

use super::ZevtcArchiveReader;
use crate::application::{
    LogMetadataParser, LogMetadataReader, MetadataError, MetadataRequest, MetadataResult,
    ParserError, StopToken,
};
use crate::domain::{EncounterMetadata, LogFileIdentity};
use crate::workers::{Processor, QueueError, WorkerOptions, WorkerPool};

pub use crate::workers::{WorkerError, WorkerSnapshot};

impl LogMetadataReader for ZevtcArchiveReader {
    fn parse(
        &self,
        file: &LogFileIdentity,
        stop: &StopToken,
    ) -> Result<EncounterMetadata, MetadataError> {
        ZevtcArchiveReader::parse(self, file, stop)
    }
}

struct ParseTask(Box<dyn LogMetadataReader>);

impl Processor for ParseTask {
    type Request = MetadataRequest;
    type Output = MetadataResult;

    fn process(&self, request: &mut MetadataRequest, stop: &StopToken) -> MetadataResult {
        MetadataResult {
            job_id: request.job_id,
            metadata: self.0.parse(&request.file, stop),
        }
    }

    fn failed(&self, request: &MetadataRequest) -> MetadataResult {
        MetadataResult {
            job_id: request.job_id,
            metadata: Err(MetadataError::Internal),
        }
    }
}

pub struct MetadataParserWorker {
    pool: WorkerPool<ParseTask>,
}

impl MetadataParserWorker {
    pub fn new(reader: impl LogMetadataReader, capacity: usize) -> Result<Self, WorkerError> {
        Ok(Self {
            pool: WorkerPool::new(
                ParseTask(Box::new(reader)),
                WorkerOptions {
                    queue_capacity: capacity,
                    parallelism: 1,
                },
            )?,
        })
    }

    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<MetadataResult> {
        self.pool.wait_for_result(timeout)
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
}

impl LogMetadataParser for MetadataParserWorker {
    fn update_queue_capacity(&mut self, capacity: usize) -> Result<(), ParserError> {
        if !(1..=64).contains(&capacity) {
            return Err(ParserError::InvalidRequest);
        }
        self.pool
            .update_queue_capacity(capacity)
            .map_err(|error| match error {
                WorkerError::Stopping => ParserError::Stopping,
                WorkerError::InvalidCapacity => ParserError::InvalidRequest,
                _ => ParserError::Unavailable,
            })
    }
    fn enqueue(&mut self, request: MetadataRequest) -> Result<(), ParserError> {
        if request.file.canonical_path.as_os_str().is_empty() {
            return Err(ParserError::InvalidRequest);
        }
        self.pool.enqueue(request).map_err(|error| match error {
            QueueError::Full => ParserError::QueueFull,
            QueueError::Stopping => ParserError::Stopping,
        })
    }

    fn available_capacity(&self) -> usize {
        self.pool.available_capacity()
    }
    fn try_take_result(&mut self) -> Result<Option<MetadataResult>, ParserError> {
        Ok(self.pool.try_take_result())
    }
    fn cancel_pending(&mut self) {
        self.pool.cancel_pending();
    }
}
