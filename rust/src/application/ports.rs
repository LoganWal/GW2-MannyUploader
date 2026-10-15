use std::time::{Duration, Instant, SystemTime};

use crate::domain::{
    DonBotUploadReceipt, DpsReportResult, EncounterMetadata, LogFileIdentity, Provider,
    TwitchDeliveryReceipt, UploadJobId, WingmanUploadReceipt,
};

pub trait Clock {
    fn system_now(&self) -> SystemTime;
    fn steady_now(&self) -> Instant;
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum UploadIntent {
    #[default]
    Automatic,
    ManualRetry,
    Reupload,
    Rechat,
}

impl UploadIntent {
    pub const fn is_user_initiated(self) -> bool {
        !matches!(self, Self::Automatic)
    }
}

/// Owned input. Provider adapters capture configuration when admitting this request.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UploadRequest {
    pub job_id: UploadJobId,
    pub provider: Provider,
    pub attempt: u32,
    pub file: LogFileIdentity,
    pub metadata: EncounterMetadata,
    pub dps_report_result: Option<DpsReportResult>,
    /// Reupload must suppress DonBot Discord delivery, including scheduled retries.
    pub intent: UploadIntent,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UploadSuccess {
    DpsReport(DpsReportResult),
    Wingman(Option<WingmanUploadReceipt>),
    DonBot(Option<DonBotUploadReceipt>),
    Twitch(TwitchDeliveryReceipt),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UploadOutcome {
    Succeeded(UploadSuccess),
    Skipped,
    /// Adapters may request a retry only when delivery is known to be safe to repeat.
    RetryAfter(Duration),
    Failed,
    TwitchDropped(TwitchDeliveryReceipt),
    /// Uncertain delivery settles as failed and never schedules an automatic retry.
    Ambiguous,
    Cancelled,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UploadResult {
    pub job_id: UploadJobId,
    pub provider: Provider,
    pub attempt: u32,
    pub outcome: UploadOutcome,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProviderError {
    QueueFull,
    Stopping,
    InvalidRequest,
    Unavailable,
    Panicked,
}

/// Calls must be nonblocking. Each adapter owns its bounded queues and independent workers.
/// Cancellation requests stop. Adapter destruction must join every owned worker.
pub trait UploadProvider: Send {
    fn provider(&self) -> Provider;
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError>;
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError>;
    fn cancel_pending(&mut self);
}
