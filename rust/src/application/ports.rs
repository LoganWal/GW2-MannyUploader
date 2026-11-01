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
    /// Explicit retries and Reupload suppress DonBot Discord delivery, including scheduled retries.
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
    SucceededWithNotice(UploadSuccess, UploadNotice),
    Skipped,
    /// Adapters may request a retry only when delivery is known to be safe to repeat.
    RetryAfter(Duration),
    Failed,
    FailedWithNotice(UploadFailure),
    TwitchDropped(TwitchDeliveryReceipt),
    /// Uncertain delivery settles as failed and never schedules an automatic retry.
    Ambiguous,
    Cancelled,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UploadNotice {
    Queued,
    AlreadyPresent,
    ProviderWarning,
    InvalidReturnedCredential,
    CredentialStorageUnavailable,
    DiscordSent,
    DiscordPartial,
    DiscordSkipped,
    DiscordFailed,
    DiscordAmbiguous,
}
impl UploadNotice {
    pub const fn detail(self) -> &'static str {
        match self {
            Self::Queued => "Queued",
            Self::AlreadyPresent => "Already present",
            Self::ProviderWarning => "Uploaded; the provider reported a warning",
            Self::InvalidReturnedCredential => "Uploaded; the returned credential was unusable",
            Self::CredentialStorageUnavailable => {
                "Uploaded; protected token storage is unavailable"
            }
            Self::DiscordSent => "Uploaded. Discord summaries sent",
            Self::DiscordPartial => "Uploaded. Some Discord summaries were not sent",
            Self::DiscordSkipped => "Uploaded. Discord summaries skipped",
            Self::DiscordFailed => "Uploaded. Discord delivery failed",
            Self::DiscordAmbiguous => "Uploaded. Discord delivery could not be confirmed",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UploadFailure {
    Twitch(super::TwitchChatError),
    DonBotKeyMissing,
    CredentialLoadFailed,
    UnconfirmedUpload,
    UnconfirmedDelivery,
}
impl UploadFailure {
    pub const fn detail(self) -> &'static str {
        match self {
            Self::Twitch(error) => error.detail(),
            Self::DonBotKeyMissing => "Verify DonBot before uploading",
            Self::CredentialLoadFailed => "Protected credentials could not be loaded",
            Self::UnconfirmedUpload => "Upload could not be confirmed. Automatic retry is disabled",
            Self::UnconfirmedDelivery => {
                "Discord delivery could not be confirmed. Automatic retry is disabled"
            }
        }
    }
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

/// Enqueue and result polling must be nonblocking. Each adapter owns bounded queues and workers.
/// Reconfiguration may allocate or start threads and belongs only to the application owner.
/// Cancellation requests stop. Adapter destruction must join every owned worker.
pub trait UploadProvider: Send {
    fn provider(&self) -> Provider;
    /// Application-owner reconfiguration. Existing queued and active requests keep captured policy.
    fn configure(
        &mut self,
        _: super::ProviderConfiguration,
    ) -> Result<(), super::ProviderConfigurationError> {
        Err(super::ProviderConfigurationError::Unavailable)
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError>;
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError>;
    fn cancel_pending(&mut self);
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MetadataError {
    FileUnavailable,
    InvalidArchive,
    UnsupportedFormat,
    MalformedLog,
    MissingPointOfView,
    ResourceLimit,
    Cancelled,
    Internal,
}

impl std::fmt::Display for MetadataError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for MetadataError {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetadataRequest {
    pub job_id: UploadJobId,
    pub file: LogFileIdentity,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetadataResult {
    pub job_id: UploadJobId,
    pub metadata: Result<EncounterMetadata, MetadataError>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParserError {
    QueueFull,
    Stopping,
    InvalidRequest,
    Unavailable,
    Panicked,
}

pub trait LogMetadataReader: Send + Sync + 'static {
    fn parse(
        &self,
        file: &LogFileIdentity,
        stop: &super::StopToken,
    ) -> Result<EncounterMetadata, MetadataError>;
}

impl<F> LogMetadataReader for F
where
    F: Fn(&LogFileIdentity, &super::StopToken) -> Result<EncounterMetadata, MetadataError>
        + Send
        + Sync
        + 'static,
{
    fn parse(
        &self,
        file: &LogFileIdentity,
        stop: &super::StopToken,
    ) -> Result<EncounterMetadata, MetadataError> {
        self(file, stop)
    }
}

/// Nonblocking metadata admission and polling. Destruction must join owned workers.
pub trait LogMetadataParser: Send {
    /// Shrinking preserves accepted requests and results while restricting new admission.
    fn update_queue_capacity(&mut self, _: usize) -> Result<(), ParserError> {
        Err(ParserError::Unavailable)
    }
    fn enqueue(&mut self, request: MetadataRequest) -> Result<(), ParserError>;
    fn available_capacity(&self) -> usize;
    fn try_take_result(&mut self) -> Result<Option<MetadataResult>, ParserError>;
    fn cancel_pending(&mut self);
}
