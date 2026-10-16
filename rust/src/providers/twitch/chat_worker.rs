use super::TwitchChatDelivery;
use crate::{
    application::*,
    domain::{Provider, TwitchDeliveryReceipt, TwitchMessageTemplate, UploadJobId},
    providers::{WorkerError, WorkerOptions, WorkerSnapshot, wire::trusted_report_permalink},
    workers::{Processor, QueueError, WorkerPool},
};
use std::{
    collections::{HashSet, VecDeque},
    sync::{Arc, Mutex},
    time::Duration,
};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchProviderConfig {
    pub message_template: String,
    pub post_success: bool,
    pub post_failure: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchProviderWorkerError {
    InvalidConfiguration,
    Worker(WorkerError),
}
type Key = (UploadJobId, String);
enum Entry {
    Sent(TwitchDeliveryReceipt),
    Ambiguous,
}
#[derive(Default)]
struct Ledger {
    settled: VecDeque<(Key, Entry)>,
    active: HashSet<Key>,
}
impl Ledger {
    fn claim(&mut self, key: &Key, manual: bool) -> Option<UploadOutcome> {
        if self.active.contains(key) {
            return Some(UploadOutcome::RetryAfter(Duration::from_secs(30)));
        }
        if !manual {
            if let Some((_, entry)) = self.settled.iter().find(|(old, _)| old == key) {
                return Some(match entry {
                    Entry::Sent(receipt) => {
                        UploadOutcome::Succeeded(UploadSuccess::Twitch(receipt.clone()))
                    }
                    Entry::Ambiguous => UploadOutcome::Ambiguous,
                });
            }
        }
        self.active.insert(key.clone());
        None
    }
    fn finish(&mut self, key: &Key, result: &TwitchChatResult) {
        self.active.remove(key);
        let entry = match result {
            TwitchChatResult::Sent(receipt) => Entry::Sent(receipt.clone()),
            TwitchChatResult::Ambiguous => Entry::Ambiguous,
            _ => return,
        };
        if let Some((_, old)) = self.settled.iter_mut().find(|(old, _)| old == key) {
            *old = entry;
        } else {
            if self.settled.len() == 256 {
                self.settled.pop_front();
            }
            self.settled.push_back((key.clone(), entry));
        }
    }
}
struct Captured {
    upload: UploadRequest,
    config: TwitchProviderConfig,
    template: TwitchMessageTemplate,
    key: Key,
    claimed: bool,
}
impl Captured {
    fn result(&self, outcome: UploadOutcome) -> UploadResult {
        UploadResult {
            job_id: self.upload.job_id,
            provider: Provider::Twitch,
            attempt: self.upload.attempt,
            outcome,
        }
    }
}
struct ChatProcessor {
    delivery: TwitchChatDelivery,
    ledger: Mutex<Ledger>,
}
impl Processor for ChatProcessor {
    type Request = Captured;
    type Output = UploadResult;
    fn process(&self, request: &mut Captured, stop: &StopToken) -> UploadResult {
        if stop.is_stop_requested() {
            return request.result(UploadOutcome::Cancelled);
        }
        let report = request
            .upload
            .dps_report_result
            .as_ref()
            .expect("admitted same-job report");
        if (report.success && !request.config.post_success)
            || (!report.success && !request.config.post_failure)
        {
            return request.result(UploadOutcome::Skipped);
        }
        let message = match request.template.render(report) {
            Ok(message) => message,
            Err(_) => {
                return request.result(UploadOutcome::FailedWithNotice(UploadFailure::Twitch(
                    TwitchChatError::InvalidMessage,
                )));
            }
        };
        {
            let mut ledger = self
                .ledger
                .lock()
                .unwrap_or_else(|poison| poison.into_inner());
            if let Some(previous) =
                ledger.claim(&request.key, request.upload.intent.is_user_initiated())
            {
                return request.result(previous);
            }
            request.claimed = true;
        }
        let delivery = self.delivery.send(&message, stop);
        self.ledger
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .finish(&request.key, &delivery);
        request.claimed = false;
        request.result(delivery.into_upload_outcome())
    }
    fn failed(&self, request: &Captured) -> UploadResult {
        if request.claimed {
            self.ledger
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .finish(&request.key, &TwitchChatResult::Ambiguous);
            request.result(UploadOutcome::Ambiguous)
        } else {
            request.result(UploadOutcome::Failed)
        }
    }
}
pub struct TwitchProviderWorker {
    pool: WorkerPool<ChatProcessor>,
    config: TwitchProviderConfig,
    template: TwitchMessageTemplate,
}
impl TwitchProviderWorker {
    pub fn new(
        client: Arc<dyn TwitchApi>,
        sessions: Arc<dyn TwitchDeliverySessionAccess>,
        config: TwitchProviderConfig,
        options: WorkerOptions,
    ) -> Result<Self, TwitchProviderWorkerError> {
        let template = TwitchMessageTemplate::parse(&config.message_template)
            .map_err(|_| TwitchProviderWorkerError::InvalidConfiguration)?;
        let pool = WorkerPool::new(
            ChatProcessor {
                delivery: TwitchChatDelivery::new(client, sessions),
                ledger: Mutex::new(Ledger::default()),
            },
            options,
        )
        .map_err(TwitchProviderWorkerError::Worker)?;
        Ok(Self {
            pool,
            config,
            template,
        })
    }
    pub fn update_config(
        &mut self,
        config: TwitchProviderConfig,
    ) -> Result<(), TwitchProviderWorkerError> {
        let template = TwitchMessageTemplate::parse(&config.message_template)
            .map_err(|_| TwitchProviderWorkerError::InvalidConfiguration)?;
        self.config = config;
        self.template = template;
        Ok(())
    }
    pub fn config_snapshot(&self) -> TwitchProviderConfig {
        self.config.clone()
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
impl UploadProvider for TwitchProviderWorker {
    fn provider(&self) -> Provider {
        Provider::Twitch
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        if request.provider != Provider::Twitch
            || request.attempt == 0
            || request.file.canonical_path.as_os_str().is_empty()
        {
            return Err(ProviderError::InvalidRequest);
        }
        let Some(report) = &request.dps_report_result else {
            return Err(ProviderError::InvalidRequest);
        };
        if !trusted_report_permalink(&report.permalink) {
            return Err(ProviderError::InvalidRequest);
        }
        let key = (request.job_id, report.permalink.clone());
        self.pool
            .enqueue(Captured {
                upload: request,
                config: self.config.clone(),
                template: self.template.clone(),
                key,
                claimed: false,
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
