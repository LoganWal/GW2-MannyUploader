use super::*;
use crate::domain::{Provider, WingmanUploadReceipt};
use crate::providers::{AsyncUploadWorker, WorkerError, WorkerOptions, WorkerSnapshot};
use std::sync::Arc;
use std::time::Duration;

pub struct WingmanProviderWorker {
    worker: AsyncUploadWorker,
}
impl WingmanProviderWorker {
    pub fn new(
        client: Arc<dyn WingmanUploader>,
        options: WorkerOptions,
    ) -> Result<Self, WorkerError> {
        let worker = AsyncUploadWorker::new(
            Provider::Wingman,
            move |request: &UploadRequest, stop: &StopToken| {
                if stop.is_stop_requested() {
                    return UploadOutcome::Cancelled;
                }
                let imported = request.dps_report_result.is_some();
                let result = match &request.dps_report_result {
                    Some(report) => client.import_permalink(&report.permalink, stop),
                    None => client.upload(&request.file, &request.metadata, stop),
                };
                if stop.is_stop_requested() {
                    return UploadOutcome::Cancelled;
                }
                match result {
                    Ok(result) => {
                        let receipt = result
                            .permalink
                            .map(|permalink| WingmanUploadReceipt { permalink });
                        let success = UploadSuccess::Wingman(receipt);
                        if result.duplicate {
                            UploadOutcome::SucceededWithNotice(
                                success,
                                UploadNotice::AlreadyPresent,
                            )
                        } else if imported {
                            UploadOutcome::SucceededWithNotice(success, UploadNotice::Queued)
                        } else {
                            UploadOutcome::Succeeded(success)
                        }
                    }
                    Err(error) => match error.disposition {
                        WingmanDisposition::Cancelled => UploadOutcome::Cancelled,
                        WingmanDisposition::Failed => UploadOutcome::Failed,
                        WingmanDisposition::Retry => UploadOutcome::RetryAfter(
                            error
                                .retry_after
                                .filter(|delay| {
                                    !delay.is_zero() && *delay <= Duration::from_secs(86400)
                                })
                                .unwrap_or(Duration::from_secs(30)),
                        ),
                    },
                }
            },
            options,
        )?;
        Ok(Self { worker })
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.worker.snapshot()
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<UploadResult> {
        self.worker.wait_for_result(timeout)
    }
    pub fn update_parallelism(&mut self, parallelism: usize) -> Result<(), WorkerError> {
        self.worker.update_parallelism(parallelism)
    }
}
impl UploadProvider for WingmanProviderWorker {
    fn provider(&self) -> Provider {
        Provider::Wingman
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        if request.metadata.boss_id == WVW_BOSS_ID {
            return Err(ProviderError::InvalidRequest);
        }
        self.worker.enqueue(request)
    }
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        self.worker.try_take_result()
    }
    fn cancel_pending(&mut self) {
        self.worker.cancel_pending();
    }
}
