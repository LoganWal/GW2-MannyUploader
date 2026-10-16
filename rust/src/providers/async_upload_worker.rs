use std::time::Duration;

use crate::application::{
    ProviderError, StopToken, UploadOutcome, UploadProvider, UploadRequest, UploadResult,
};
use crate::domain::Provider;
use crate::workers::{Processor, QueueError, WorkerPool};
pub use crate::workers::{WorkerError, WorkerOptions, WorkerSnapshot};

/// Protocol work runs outside queue locks and must cooperate with cancellation.
pub trait UploadProcessor: Send + Sync + 'static {
    fn process(&self, request: &UploadRequest, stop: &StopToken) -> UploadOutcome;
}

impl<F> UploadProcessor for F
where
    F: Fn(&UploadRequest, &StopToken) -> UploadOutcome + Send + Sync + 'static,
{
    fn process(&self, request: &UploadRequest, stop: &StopToken) -> UploadOutcome {
        self(request, stop)
    }
}

struct UploadTask(Box<dyn UploadProcessor>);

impl Processor for UploadTask {
    type Request = UploadRequest;
    type Output = UploadResult;

    fn process(&self, request: &UploadRequest, stop: &StopToken) -> UploadResult {
        Self::result(request, self.0.process(request, stop))
    }

    fn failed(&self, request: &UploadRequest) -> UploadResult {
        Self::result(request, UploadOutcome::Failed)
    }
}

impl UploadTask {
    fn result(request: &UploadRequest, outcome: UploadOutcome) -> UploadResult {
        UploadResult {
            job_id: request.job_id,
            provider: request.provider,
            attempt: request.attempt,
            outcome,
        }
    }
}

/// One bounded, independently sized provider pool. Destruction cancels and joins every thread.
pub struct AsyncUploadWorker {
    provider: Provider,
    pool: WorkerPool<UploadTask>,
}

impl AsyncUploadWorker {
    pub fn new(
        provider: Provider,
        processor: impl UploadProcessor,
        options: WorkerOptions,
    ) -> Result<Self, WorkerError> {
        Ok(Self {
            provider,
            pool: WorkerPool::new(UploadTask(Box::new(processor)), options)?,
        })
    }

    pub fn update_parallelism(&mut self, parallelism: usize) -> Result<(), WorkerError> {
        self.pool.update_parallelism(parallelism)
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<UploadResult> {
        self.pool.wait_for_result(timeout)
    }
}

impl UploadProvider for AsyncUploadWorker {
    fn provider(&self) -> Provider {
        self.provider
    }

    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        if request.provider != self.provider
            || request.attempt == 0
            || request.file.canonical_path.as_os_str().is_empty()
        {
            return Err(ProviderError::InvalidRequest);
        }
        self.pool.enqueue(request).map_err(|error| match error {
            QueueError::Full => ProviderError::QueueFull,
            QueueError::Stopping => ProviderError::Stopping,
        })
    }

    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        Ok(self.pool.try_take_result())
    }
    fn cancel_pending(&mut self) {
        self.pool.cancel_pending();
    }
}
