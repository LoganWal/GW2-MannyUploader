use super::*;
use crate::domain::{
    DonBotDiscordDeliveryOutcome, DonBotUploadReceipt, Provider, canonical_discord_id,
};
use crate::providers::{WorkerError, WorkerOptions, WorkerSnapshot};
use crate::workers::{Processor, QueueError, WorkerPool};
use std::sync::Mutex;

pub(super) type Secrets = Option<Arc<Mutex<Box<dyn SecretStore>>>>;
pub(super) fn load_key(secrets: &Secrets) -> Result<SecretValue, UploadFailure> {
    let secrets = secrets.as_ref().ok_or(UploadFailure::DonBotKeyMissing)?;
    let store = secrets
        .lock()
        .map_err(|_| UploadFailure::CredentialLoadFailed)?;
    store.load(SecretId::DonBotGw2ApiKey).map_err(|error| {
        if error.code == SecretStoreErrorCode::NotFound {
            UploadFailure::DonBotKeyMissing
        } else {
            UploadFailure::CredentialLoadFailed
        }
    })
}
pub(super) fn normalize_options(mut options: DonBotOptions) -> Option<DonBotOptions> {
    if !options.delivery.is_valid()
        || if options.guild_id.is_empty() {
            options.delivery.requested()
        } else {
            !canonical_discord_id(&options.guild_id)
        }
    {
        return None;
    }
    options.api_base_url = normalize_donbot_api_base(&options.api_base_url)?;
    Some(options)
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotWorkerError {
    InvalidConfiguration,
    Worker(WorkerError),
}
struct Captured {
    request: UploadRequest,
    options: DonBotOptions,
}
struct Upload {
    client: Arc<dyn DonBotUploader>,
    secrets: Secrets,
}
impl Upload {
    fn execute(&self, captured: &Captured, stop: &StopToken) -> UploadOutcome {
        if stop.is_stop_requested() {
            return UploadOutcome::Cancelled;
        }
        let key = match load_key(&self.secrets) {
            Ok(key) => key,
            Err(reason) => return UploadOutcome::FailedWithNotice(reason),
        };
        if stop.is_stop_requested() {
            return UploadOutcome::Cancelled;
        }
        let request = &captured.request;
        let result = match &request.dps_report_result {
            Some(report) => {
                self.client
                    .import_permalink(&report.permalink, &captured.options, &key, stop)
            }
            None => self
                .client
                .upload(&request.file, &captured.options, &key, stop),
        };
        if stop.is_stop_requested() {
            return UploadOutcome::Cancelled;
        }
        match result {
            Err(error) => match error.disposition {
                DonBotDisposition::Cancelled => UploadOutcome::Cancelled,
                DonBotDisposition::Retry => UploadOutcome::RetryAfter(
                    error
                        .retry_after
                        .filter(|delay| !delay.is_zero() && *delay <= Duration::from_secs(86400))
                        .unwrap_or(Duration::from_secs(30)),
                ),
                DonBotDisposition::Failed => match error.code {
                    DonBotErrorCode::UnconfirmedUpload => {
                        UploadOutcome::FailedWithNotice(UploadFailure::UnconfirmedUpload)
                    }
                    DonBotErrorCode::UnconfirmedDelivery => {
                        UploadOutcome::FailedWithNotice(UploadFailure::UnconfirmedDelivery)
                    }
                    DonBotErrorCode::InvalidKey => {
                        UploadOutcome::FailedWithNotice(UploadFailure::DonBotKeyMissing)
                    }
                    _ => UploadOutcome::Failed,
                },
            },
            Ok(result) => {
                if result
                    .upload_id
                    .is_some_and(|id| !super::upload_response::positive_id(id))
                    || result
                        .fight_log_id
                        .is_some_and(|id| !super::upload_response::positive_id(id))
                    || !result.discord_delivery.is_valid_with_limit(4)
                    || captured.options.delivery.requested()
                        != (result.discord_delivery.outcome
                            != DonBotDiscordDeliveryOutcome::NotRequested)
                {
                    return UploadOutcome::Failed;
                }
                let notice = match result.discord_delivery.outcome {
                    DonBotDiscordDeliveryOutcome::Sent => Some(UploadNotice::DiscordSent),
                    DonBotDiscordDeliveryOutcome::Partial => Some(UploadNotice::DiscordPartial),
                    DonBotDiscordDeliveryOutcome::Skipped => Some(UploadNotice::DiscordSkipped),
                    DonBotDiscordDeliveryOutcome::Failed => Some(UploadNotice::DiscordFailed),
                    DonBotDiscordDeliveryOutcome::Ambiguous => Some(UploadNotice::DiscordAmbiguous),
                    DonBotDiscordDeliveryOutcome::NotRequested
                        if request.dps_report_result.is_some() && result.fight_log_id.is_none() =>
                    {
                        Some(UploadNotice::Queued)
                    }
                    _ => None,
                };
                let receipt = DonBotUploadReceipt {
                    upload_id: result.upload_id,
                    fight_log_id: result.fight_log_id,
                    discord_delivery: result.discord_delivery,
                    guild_id: Some(captured.options.guild_id.clone()),
                };
                let success = UploadSuccess::DonBot(Some(receipt));
                match notice {
                    Some(notice) => UploadOutcome::SucceededWithNotice(success, notice),
                    None => UploadOutcome::Succeeded(success),
                }
            }
        }
    }
    fn result(request: &Captured, outcome: UploadOutcome) -> UploadResult {
        UploadResult {
            job_id: request.request.job_id,
            provider: Provider::DonBot,
            attempt: request.request.attempt,
            outcome,
        }
    }
}
impl Processor for Upload {
    type Request = Captured;
    type Output = UploadResult;
    fn process(&self, request: &mut Captured, stop: &StopToken) -> UploadResult {
        Self::result(request, self.execute(request, stop))
    }
    fn failed(&self, request: &Captured) -> UploadResult {
        Self::result(request, UploadOutcome::Failed)
    }
}
pub struct DonBotProviderWorker {
    pool: WorkerPool<Upload>,
    options: DonBotOptions,
}
impl DonBotProviderWorker {
    pub fn new(
        client: Arc<dyn DonBotUploader>,
        secrets: Secrets,
        options: DonBotOptions,
        worker: WorkerOptions,
    ) -> Result<Self, DonBotWorkerError> {
        let options = normalize_options(options).ok_or(DonBotWorkerError::InvalidConfiguration)?;
        Ok(Self {
            pool: WorkerPool::new(Upload { client, secrets }, worker)
                .map_err(DonBotWorkerError::Worker)?,
            options,
        })
    }
    pub fn update_config(&mut self, options: DonBotOptions) -> Result<(), DonBotWorkerError> {
        self.options = normalize_options(options).ok_or(DonBotWorkerError::InvalidConfiguration)?;
        Ok(())
    }
    pub fn config_snapshot(&self) -> DonBotOptions {
        self.options.clone()
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
impl UploadProvider for DonBotProviderWorker {
    fn configure(
        &mut self,
        configuration: ProviderConfiguration,
    ) -> Result<(), ProviderConfigurationError> {
        let ProviderPolicy::DonBot {
            api_base_url,
            guild_id,
            delivery,
        } = configuration.policy
        else {
            return Err(ProviderConfigurationError::InvalidConfiguration);
        };
        let options = normalize_options(DonBotOptions {
            api_base_url,
            guild_id,
            delivery,
        })
        .ok_or(ProviderConfigurationError::InvalidConfiguration)?;
        self.pool.update_parallelism(configuration.parallelism)?;
        self.options = options;
        Ok(())
    }
    fn provider(&self) -> Provider {
        Provider::DonBot
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        if request.provider != Provider::DonBot
            || request.attempt == 0
            || request.file.canonical_path.as_os_str().is_empty()
            || self.options.guild_id.is_empty()
            || request.intent == UploadIntent::Rechat
        {
            return Err(ProviderError::InvalidRequest);
        }
        let mut options = self.options.clone();
        if request.intent.is_user_initiated() {
            options.delivery = DonBotDeliveryRoute::None;
        }
        self.pool
            .enqueue(Captured { request, options })
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
