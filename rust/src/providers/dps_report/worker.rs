use std::sync::{Arc, Mutex};
use std::time::Duration;

use super::*;
use crate::domain::Provider;
use crate::providers::{WorkerError, WorkerOptions, WorkerSnapshot};
use crate::workers::{Processor, QueueError, WorkerPool};

struct CapturedUpload {
    upload: UploadRequest,
    options: DpsReportOptions,
}
struct DpsProcessor {
    client: Arc<dyn DpsReportUploader>,
    secrets: Option<Arc<Mutex<Box<dyn SecretStore>>>>,
}
impl DpsProcessor {
    fn upload(&self, request: &CapturedUpload, stop: &StopToken) -> UploadOutcome {
        if stop.is_stop_requested() {
            return UploadOutcome::Cancelled;
        }
        let current = if let Some(secrets) = &self.secrets {
            let Ok(store) = secrets.lock() else {
                return UploadOutcome::Failed;
            };
            match store.load(SecretId::DpsReportUserToken) {
                Ok(secret) => Some(secret),
                Err(error) if error.code == SecretStoreErrorCode::NotFound => None,
                Err(_) => return UploadOutcome::Failed,
            }
        } else {
            None
        };
        if stop.is_stop_requested() {
            return UploadOutcome::Cancelled;
        }
        let result = self.client.upload(
            &request.upload.file,
            current.as_ref(),
            request.options,
            stop,
        );
        if stop.is_stop_requested() {
            return UploadOutcome::Cancelled;
        }
        match result {
            Ok(success) => {
                let mut warning = success.warning.map(|warning| match warning {
                    DpsReportWarning::GeneratedWithWarning => UploadNotice::ProviderWarning,
                    DpsReportWarning::InvalidReturnedToken => {
                        UploadNotice::InvalidReturnedCredential
                    }
                });
                if let Some(replacement) = success.replacement_user_token {
                    if let Some(secrets) = &self.secrets {
                        let Ok(mut store) = secrets.lock() else {
                            return UploadOutcome::Failed;
                        };
                        if stop.is_stop_requested() {
                            return UploadOutcome::Cancelled;
                        }
                        if store
                            .store(SecretId::DpsReportUserToken, &replacement)
                            .is_err()
                        {
                            return UploadOutcome::Failed;
                        }
                    } else {
                        warning = Some(UploadNotice::CredentialStorageUnavailable);
                    }
                }
                let result = UploadSuccess::DpsReport(success.report);
                match warning {
                    Some(warning) => UploadOutcome::SucceededWithNotice(result, warning),
                    None => UploadOutcome::Succeeded(result),
                }
            }
            Err(error) => match error.disposition {
                DpsReportDisposition::Cancelled => UploadOutcome::Cancelled,
                DpsReportDisposition::Failed => UploadOutcome::Failed,
                DpsReportDisposition::Retry => UploadOutcome::RetryAfter(
                    error
                        .retry_after
                        .filter(|delay| !delay.is_zero() && *delay <= Duration::from_secs(86400))
                        .unwrap_or(Duration::from_secs(30)),
                ),
            },
        }
    }
    fn result(request: &CapturedUpload, outcome: UploadOutcome) -> UploadResult {
        UploadResult {
            job_id: request.upload.job_id,
            provider: Provider::DpsReport,
            attempt: request.upload.attempt,
            outcome,
        }
    }
}
impl Processor for DpsProcessor {
    type Request = CapturedUpload;
    type Output = UploadResult;
    fn process(&self, request: &mut CapturedUpload, stop: &StopToken) -> UploadResult {
        Self::result(request, self.upload(request, stop))
    }
    fn failed(&self, request: &CapturedUpload) -> UploadResult {
        Self::result(request, UploadOutcome::Failed)
    }
}

pub struct DpsReportProviderWorker {
    pool: WorkerPool<DpsProcessor>,
    options: DpsReportOptions,
}
impl DpsReportProviderWorker {
    pub fn new(
        client: Arc<dyn DpsReportUploader>,
        secrets: Option<Arc<Mutex<Box<dyn SecretStore>>>>,
        worker: WorkerOptions,
        options: DpsReportOptions,
    ) -> Result<Self, WorkerError> {
        Ok(Self {
            pool: WorkerPool::new(DpsProcessor { client, secrets }, worker)?,
            options,
        })
    }
    pub fn update_config(&mut self, options: DpsReportOptions) {
        self.options = options;
    }
    pub fn config_snapshot(&self) -> DpsReportOptions {
        self.options
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
    pub fn update_parallelism(&mut self, parallelism: usize) -> Result<(), WorkerError> {
        self.pool.update_parallelism(parallelism)
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<UploadResult> {
        self.pool.wait_for_result(timeout)
    }
}
impl UploadProvider for DpsReportProviderWorker {
    fn provider(&self) -> Provider {
        Provider::DpsReport
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        if request.provider != Provider::DpsReport
            || request.attempt == 0
            || request.file.canonical_path.as_os_str().is_empty()
            || request.dps_report_result.is_some()
        {
            return Err(ProviderError::InvalidRequest);
        }
        self.pool
            .enqueue(CapturedUpload {
                upload: request,
                options: self.options,
            })
            .map_err(|error| match error {
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
