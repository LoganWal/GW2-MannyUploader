use std::num::NonZeroU64;
use std::path::PathBuf;
use std::time::{Instant, SystemTime};

use super::{
    DonBotUploadReceipt, DpsReportResult, EncounterMetadata, TwitchDeliveryReceipt,
    TwitchDeliveryStatus, WingmanUploadReceipt,
};

pub const WVW_BOSS_ID: u16 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Provider {
    DpsReport,
    Wingman,
    DonBot,
    Twitch,
}

impl Provider {
    pub const ALL: [Self; 4] = [Self::DpsReport, Self::Wingman, Self::DonBot, Self::Twitch];

    pub const fn name(self) -> &'static str {
        match self {
            Self::DpsReport => "dps.report",
            Self::Wingman => "GW2Wingman",
            Self::DonBot => "DonBot",
            Self::Twitch => "Twitch",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProviderSelection {
    pub dps_report: bool,
    pub wingman: bool,
    pub donbot: bool,
    pub twitch: bool,
}

impl ProviderSelection {
    pub const fn contains(self, provider: Provider) -> bool {
        match provider {
            Provider::DpsReport => self.dps_report,
            Provider::Wingman => self.wingman,
            Provider::DonBot => self.donbot,
            Provider::Twitch => self.twitch,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProviderState {
    #[default]
    Disabled,
    Waiting,
    Active,
    Succeeded,
    Skipped,
    RetryScheduled(Instant),
    Failed,
    Cancelled,
}

impl ProviderState {
    pub const fn is_busy(self) -> bool {
        matches!(self, Self::Waiting | Self::Active | Self::RetryScheduled(_))
    }

    pub const fn is_terminal(self) -> bool {
        // Failed remains eligible for the separate manual retry action.
        matches!(
            self,
            Self::Disabled | Self::Succeeded | Self::Skipped | Self::Cancelled
        )
    }

    fn allows(self, next: Self) -> bool {
        match self {
            Self::Waiting => matches!(
                next,
                Self::Active | Self::Skipped | Self::Failed | Self::Cancelled
            ),
            Self::Active => matches!(
                next,
                Self::Succeeded | Self::RetryScheduled(_) | Self::Failed | Self::Cancelled
            ),
            Self::RetryScheduled(_) => {
                matches!(next, Self::Active | Self::Failed | Self::Cancelled)
            }
            Self::Failed => matches!(next, Self::Waiting | Self::Cancelled),
            _ => false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct UploadJobId(NonZeroU64);

impl UploadJobId {
    pub fn new(value: u64) -> Result<Self, JobError> {
        NonZeroU64::new(value)
            .map(Self)
            .ok_or(JobError::InvalidJobId)
    }

    pub const fn get(self) -> u64 {
        self.0.get()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LogFileIdentity {
    pub canonical_path: PathBuf,
    pub size: u64,
    pub last_write_time: SystemTime,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProviderStatus {
    pub state: ProviderState,
    pub attempts: u32,
    /// Locally generated safe text, never raw provider errors or paths.
    pub detail: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum JobError {
    InvalidJobId,
    EmptyPath,
    TwitchRequiresDpsReport,
    InvalidTransition,
    DpsReportResultRequired,
    EmptyPermalink,
    MetadataAlreadySet,
    InvalidTwitchDelivery,
    TwitchDeliveryAlreadyRecorded,
    InvalidDonBotUpload,
    DonBotUploadAlreadyRecorded,
    InvalidWingmanUpload,
    WingmanUploadAlreadyRecorded,
    ManualRetryRequiresFailure,
    ExplicitDeliveryBusy,
    EncounterMetadataRequired,
    AttemptLimitReached,
}

impl std::fmt::Display for JobError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // Error variants contain no caller-controlled or sensitive values.
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for JobError {}

/// An in-memory snapshot. Persistence must validate its schema and strip retry deadlines.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UploadJobRecord {
    pub file: LogFileIdentity,
    pub detected_at: SystemTime,
    pub encounter_metadata: Option<EncounterMetadata>,
    pub dps_report_result: Option<DpsReportResult>,
    pub wingman_upload_receipt: Option<WingmanUploadReceipt>,
    pub donbot_upload_receipt: Option<DonBotUploadReceipt>,
    pub twitch_delivery_receipt: Option<TwitchDeliveryReceipt>,
    pub providers: [ProviderStatus; 4],
}

pub struct UploadJob {
    id: UploadJobId,
    record: UploadJobRecord,
}

impl UploadJob {
    pub fn new(
        id: UploadJobId,
        file: LogFileIdentity,
        detected_at: SystemTime,
        enabled: ProviderSelection,
    ) -> Result<Self, JobError> {
        if file.canonical_path.as_os_str().is_empty() {
            return Err(JobError::EmptyPath);
        }
        if enabled.twitch && !enabled.dps_report {
            return Err(JobError::TwitchRequiresDpsReport);
        }
        Ok(Self {
            id,
            record: UploadJobRecord {
                file,
                detected_at,
                encounter_metadata: None,
                dps_report_result: None,
                wingman_upload_receipt: None,
                donbot_upload_receipt: None,
                twitch_delivery_receipt: None,
                providers: Provider::ALL.map(|provider| ProviderStatus {
                    state: if enabled.contains(provider) {
                        ProviderState::Waiting
                    } else {
                        ProviderState::Disabled
                    },
                    ..ProviderStatus::default()
                }),
            },
        })
    }

    pub fn restore(id: UploadJobId, mut record: UploadJobRecord) -> Result<Self, JobError> {
        if record.file.canonical_path.as_os_str().is_empty() {
            return Err(JobError::EmptyPath);
        }
        if record
            .dps_report_result
            .as_ref()
            .is_some_and(|result| result.permalink.is_empty())
        {
            return Err(JobError::EmptyPermalink);
        }
        if record
            .wingman_upload_receipt
            .as_ref()
            .is_some_and(|receipt| receipt.permalink.is_empty())
        {
            return Err(JobError::InvalidWingmanUpload);
        }
        if record
            .donbot_upload_receipt
            .as_ref()
            .is_some_and(|receipt| !receipt.is_valid())
        {
            return Err(JobError::InvalidDonBotUpload);
        }
        if record
            .twitch_delivery_receipt
            .as_ref()
            .is_some_and(|receipt| !receipt.is_valid())
        {
            return Err(JobError::InvalidTwitchDelivery);
        }
        for status in &mut record.providers {
            if status.state.is_busy() {
                status.state = ProviderState::Failed;
                status.detail = "Interrupted by the previous game session".into();
            }
        }
        Ok(Self { id, record })
    }

    pub const fn id(&self) -> UploadJobId {
        self.id
    }

    pub fn snapshot(&self) -> &UploadJobRecord {
        &self.record
    }

    pub fn provider_status(&self, provider: Provider) -> &ProviderStatus {
        &self.record.providers[provider as usize]
    }

    pub fn set_encounter_metadata(&mut self, metadata: EncounterMetadata) -> Result<(), JobError> {
        if self.record.encounter_metadata.is_some() {
            return Err(JobError::MetadataAlreadySet);
        }
        self.record.encounter_metadata = Some(metadata);
        Ok(())
    }

    /// Routing dependencies for Wingman and DonBot belong to the coordinator.
    pub fn transition(
        &mut self,
        provider: Provider,
        next: ProviderState,
        detail: String,
    ) -> Result<(), JobError> {
        if !self.provider_status(provider).state.allows(next) {
            return Err(JobError::InvalidTransition);
        }
        if provider == Provider::DpsReport && next == ProviderState::Succeeded {
            return Err(JobError::DpsReportResultRequired);
        }
        if provider == Provider::Twitch {
            if next == ProviderState::Active
                && (self.provider_status(Provider::DpsReport).state != ProviderState::Succeeded
                    || self.record.dps_report_result.is_none())
            {
                return Err(JobError::DpsReportResultRequired);
            }
            if next == ProviderState::Succeeded
                && !self
                    .record
                    .twitch_delivery_receipt
                    .as_ref()
                    .is_some_and(|receipt| receipt.status == TwitchDeliveryStatus::Sent)
            {
                return Err(JobError::InvalidTwitchDelivery);
            }
        }
        let status = &mut self.record.providers[provider as usize];
        let attempts = if next == ProviderState::Active {
            status
                .attempts
                .checked_add(1)
                .ok_or(JobError::AttemptLimitReached)?
        } else {
            status.attempts
        };
        status.state = next;
        status.attempts = attempts;
        status.detail = if next == ProviderState::Waiting {
            String::new()
        } else {
            detail
        };
        Ok(())
    }

    pub fn complete_dps_report(
        &mut self,
        result: DpsReportResult,
        detail: String,
    ) -> Result<(), JobError> {
        self.require_active(Provider::DpsReport)?;
        if result.permalink.is_empty() {
            return Err(JobError::EmptyPermalink);
        }
        self.record.dps_report_result = Some(result);
        let status = &mut self.record.providers[Provider::DpsReport as usize];
        status.state = ProviderState::Succeeded;
        status.detail = detail;
        Ok(())
    }

    pub fn record_wingman_upload(&mut self, receipt: WingmanUploadReceipt) -> Result<(), JobError> {
        self.require_active(Provider::Wingman)?;
        if self.record.wingman_upload_receipt.is_some() {
            return Err(JobError::WingmanUploadAlreadyRecorded);
        }
        if receipt.permalink.is_empty() {
            return Err(JobError::InvalidWingmanUpload);
        }
        self.record.wingman_upload_receipt = Some(receipt);
        Ok(())
    }

    pub fn record_donbot_upload(&mut self, receipt: DonBotUploadReceipt) -> Result<(), JobError> {
        self.require_active(Provider::DonBot)?;
        if self.record.donbot_upload_receipt.is_some() {
            return Err(JobError::DonBotUploadAlreadyRecorded);
        }
        if !receipt.is_valid() {
            return Err(JobError::InvalidDonBotUpload);
        }
        self.record.donbot_upload_receipt = Some(receipt);
        Ok(())
    }

    pub fn record_twitch_delivery(
        &mut self,
        receipt: TwitchDeliveryReceipt,
    ) -> Result<(), JobError> {
        self.require_active(Provider::Twitch)?;
        if self.record.twitch_delivery_receipt.is_some() {
            return Err(JobError::TwitchDeliveryAlreadyRecorded);
        }
        if !receipt.is_valid() {
            return Err(JobError::InvalidTwitchDelivery);
        }
        self.record.twitch_delivery_receipt = Some(receipt);
        Ok(())
    }

    pub fn prepare_manual_retry(&mut self, provider: Provider) -> Result<(), JobError> {
        if self.provider_status(provider).state != ProviderState::Failed {
            return Err(JobError::ManualRetryRequiresFailure);
        }
        self.transition(provider, ProviderState::Waiting, String::new())?;
        if provider == Provider::Twitch {
            self.record.twitch_delivery_receipt = None;
        }
        if provider == Provider::DpsReport && self.record.dps_report_result.is_none() {
            for dependent in [Provider::Wingman, Provider::DonBot, Provider::Twitch] {
                let status = &mut self.record.providers[dependent as usize];
                if status.state == ProviderState::Skipped {
                    status.state = ProviderState::Waiting;
                    status.detail.clear();
                }
            }
        }
        Ok(())
    }

    /// The coordinator must preflight every upload destination before a grouped Reupload.
    pub fn prepare_explicit_delivery(&mut self, provider: Provider) -> Result<(), JobError> {
        if self.record.encounter_metadata.is_none() {
            return Err(JobError::EncounterMetadataRequired);
        }
        if self.provider_status(provider).state.is_busy() {
            return Err(JobError::ExplicitDeliveryBusy);
        }
        if provider == Provider::Twitch && self.record.dps_report_result.is_none() {
            return Err(JobError::DpsReportResultRequired);
        }
        let status = &mut self.record.providers[provider as usize];
        status.state = ProviderState::Waiting;
        status.detail.clear();
        match provider {
            Provider::DpsReport => self.record.dps_report_result = None,
            Provider::Wingman => self.record.wingman_upload_receipt = None,
            Provider::DonBot => self.record.donbot_upload_receipt = None,
            Provider::Twitch => self.record.twitch_delivery_receipt = None,
        }
        Ok(())
    }

    fn require_active(&self, provider: Provider) -> Result<(), JobError> {
        if self.provider_status(provider).state == ProviderState::Active {
            Ok(())
        } else {
            Err(JobError::InvalidTransition)
        }
    }
}
