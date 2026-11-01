use super::*;
use crate::providers::{WorkerError, WorkerOptions, WorkerSnapshot};
use crate::workers::{Processor, QueueError, WorkerPool};

struct Verify(Arc<dyn DonBotIdentityClient>);
impl Processor for Verify {
    type Request = DonBotVerificationRequest;
    type Output = DonBotVerificationResult;
    fn process(&self, request: &mut Self::Request, stop: &StopToken) -> Self::Output {
        let api_key = std::mem::take(&mut request.api_key);
        let verification = if stop.is_stop_requested() {
            Err(DonBotVerificationFailure::Cancelled)
        } else {
            let result = self.0.verify(&request.api_base_url, &api_key, stop);
            if stop.is_stop_requested() {
                Err(DonBotVerificationFailure::Cancelled)
            } else {
                match result {
                    Ok(identity) => Ok(DonBotVerificationSuccess { identity, api_key }),
                    Err(error) if error.disposition == DonBotDisposition::Cancelled => {
                        Err(DonBotVerificationFailure::Cancelled)
                    }
                    Err(_) => Err(DonBotVerificationFailure::Failed),
                }
            }
        };
        DonBotVerificationResult {
            request_id: request.request_id,
            verification,
        }
    }
    fn failed(&self, request: &Self::Request) -> Self::Output {
        DonBotVerificationResult {
            request_id: request.request_id,
            verification: Err(DonBotVerificationFailure::Failed),
        }
    }
}

pub struct DonBotVerificationWorker {
    pool: WorkerPool<Verify>,
}
impl DonBotVerificationWorker {
    pub fn new(
        client: Arc<dyn DonBotIdentityClient>,
        capacity: usize,
    ) -> Result<Self, WorkerError> {
        Ok(Self {
            pool: WorkerPool::new(
                Verify(client),
                WorkerOptions {
                    queue_capacity: capacity,
                    parallelism: 1,
                },
            )?,
        })
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<DonBotVerificationResult> {
        self.pool.wait_for_result(timeout)
    }
}
impl DonBotVerifier for DonBotVerificationWorker {
    fn enqueue(
        &mut self,
        mut request: DonBotVerificationRequest,
    ) -> Result<(), DonBotVerificationDispatchError> {
        if request.request_id == 0 || !valid_donbot_api_key(&request.api_key) {
            return Err(DonBotVerificationDispatchError::InvalidRequest);
        }
        request.api_base_url = normalize_donbot_api_base(&request.api_base_url)
            .ok_or(DonBotVerificationDispatchError::InvalidRequest)?;
        self.pool.enqueue(request).map_err(|error| match error {
            QueueError::Full => DonBotVerificationDispatchError::QueueFull,
            QueueError::Stopping => DonBotVerificationDispatchError::Stopping,
        })
    }
    fn try_take_result(&mut self) -> Option<DonBotVerificationResult> {
        self.pool.try_take_result()
    }
    fn cancel_pending(&mut self) {
        self.pool.cancel_pending();
    }
}
