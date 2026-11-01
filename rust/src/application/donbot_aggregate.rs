use std::{
    collections::HashSet,
    panic::{AssertUnwindSafe, catch_unwind},
};

use super::{
    Clock, ConfigurationService, DonBotAggregateDelivery, DonBotAggregateOutcome,
    DonBotAggregateRequest, DonBotAggregateResult, DonBotConfigurationSnapshot,
    MAX_DONBOT_AGGREGATE_FIGHTS, UploadCoordinator, UploadFailure,
    authorized_donbot_aggregate_route,
};
use crate::domain::{DonBotDiscordDeliveryOutcome, DonBotDiscordDeliveryReceipt, UploadJobId};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SendDonBotAggregateCommand {
    pub job_ids: Vec<UploadJobId>,
    pub configuration_revision: u64,
    pub donbot_revision: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotAggregateDeliveryState {
    Idle,
    Queued,
    Sending,
    Succeeded,
    Failed,
    Ambiguous,
    Cancelled,
    ShuttingDown,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotAggregateDeliveryError {
    InvalidCommand,
    Busy,
    StaleConfiguration,
    Unavailable,
    UnknownJob,
    IneligibleJob,
    DispatchFailed,
    ShuttingDown,
    RevisionExhausted,
}
impl DonBotAggregateDeliveryError {
    pub fn detail(self) -> &'static str {
        match self {
            Self::InvalidCommand => "The DonBot aggregate selection is invalid",
            Self::Busy => "A DonBot aggregate delivery is already active",
            Self::StaleConfiguration => "DonBot settings changed before aggregate delivery started",
            Self::Unavailable => "DonBot aggregate delivery is unavailable for the selected server",
            Self::UnknownJob => "A selected log is no longer retained",
            Self::IneligibleJob => "A selected log has no unique completed DonBot result",
            Self::DispatchFailed => "DonBot aggregate delivery could not be queued",
            Self::ShuttingDown => "DonBot aggregate delivery is shutting down",
            Self::RevisionExhausted => "DonBot aggregate delivery cannot accept more requests",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotAggregateDeliverySnapshot {
    pub state: DonBotAggregateDeliveryState,
    pub fight_log_count: usize,
    pub discord_delivery: Option<DonBotDiscordDeliveryReceipt>,
    pub diagnostic: Option<DonBotAggregateDeliveryError>,
    pub credential_failure: Option<UploadFailure>,
    pub revision: u64,
    pub shutting_down: bool,
}
impl DonBotAggregateDeliverySnapshot {
    pub fn detail(&self) -> &'static str {
        if let Some(error) = self.diagnostic {
            return error.detail();
        }
        if let Some(error) = self.credential_failure {
            return error.detail();
        }
        match self.state {
            DonBotAggregateDeliveryState::Idle | DonBotAggregateDeliveryState::ShuttingDown => "",
            DonBotAggregateDeliveryState::Queued => "DonBot aggregate delivery is queued",
            DonBotAggregateDeliveryState::Sending => "Sending DonBot aggregate",
            DonBotAggregateDeliveryState::Succeeded => "DonBot aggregate delivery completed",
            DonBotAggregateDeliveryState::Failed => "DonBot aggregate delivery failed",
            DonBotAggregateDeliveryState::Ambiguous => {
                "DonBot aggregate delivery could not be confirmed. Automatic retry is disabled"
            }
            DonBotAggregateDeliveryState::Cancelled => "DonBot aggregate delivery was cancelled",
        }
    }
}

/// Application-owner controller. Render callbacks enqueue value commands through the runtime.
/// Aggregate state stays in this session and never mutates the upload coordinator or its receipts.
pub struct DonBotAggregateDeliveryController {
    delivery: Box<dyn DonBotAggregateDelivery>,
    command: Option<SendDonBotAggregateCommand>,
    active_request: Option<u64>,
    next_request: u64,
    snapshot: DonBotAggregateDeliverySnapshot,
}
impl DonBotAggregateDeliveryController {
    pub fn new(delivery: Box<dyn DonBotAggregateDelivery>) -> Self {
        Self {
            delivery,
            command: None,
            active_request: None,
            next_request: 1,
            snapshot: DonBotAggregateDeliverySnapshot {
                state: DonBotAggregateDeliveryState::Idle,
                fight_log_count: 0,
                discord_delivery: None,
                diagnostic: None,
                credential_failure: None,
                revision: 1,
                shutting_down: false,
            },
        }
    }
    pub fn snapshot(&self) -> DonBotAggregateDeliverySnapshot {
        self.snapshot.clone()
    }

    pub fn submit(
        &mut self,
        command: SendDonBotAggregateCommand,
    ) -> Result<(), DonBotAggregateDeliveryError> {
        if self.snapshot.shutting_down {
            return Err(DonBotAggregateDeliveryError::ShuttingDown);
        }
        if self.command.is_some() || self.active_request.is_some() {
            return Err(DonBotAggregateDeliveryError::Busy);
        }
        // Reserve queued, sending, and completion revisions, followed by shutdown.
        if self.snapshot.revision >= u64::MAX - 3 || self.next_request == u64::MAX {
            return Err(DonBotAggregateDeliveryError::RevisionExhausted);
        }
        if command.configuration_revision == 0
            || command.donbot_revision == 0
            || !(2..=usize::from(MAX_DONBOT_AGGREGATE_FIGHTS)).contains(&command.job_ids.len())
        {
            return Err(DonBotAggregateDeliveryError::InvalidCommand);
        }
        let mut unique = HashSet::with_capacity(command.job_ids.len());
        if command.job_ids.iter().any(|id| !unique.insert(*id)) {
            return Err(DonBotAggregateDeliveryError::InvalidCommand);
        }
        self.snapshot.fight_log_count = command.job_ids.len();
        self.command = Some(command);
        self.publish(DonBotAggregateDeliveryState::Queued, None, None, None);
        Ok(())
    }

    /// Consumes at most one result and dispatches at most one explicitly submitted command.
    pub fn tick<C: Clock>(
        &mut self,
        uploads: &UploadCoordinator<C>,
        configuration: &ConfigurationService,
        donbot: &DonBotConfigurationSnapshot,
    ) -> Result<(), DonBotAggregateDeliveryError> {
        if self.snapshot.shutting_down || configuration.is_shutting_down() {
            return Err(DonBotAggregateDeliveryError::ShuttingDown);
        }
        match catch_unwind(AssertUnwindSafe(|| self.delivery.try_take_result())) {
            Ok(Some(result)) => self.handle_result(result),
            Ok(None) => (),
            Err(_) => {
                if self.active_request.take().is_some() {
                    self.publish(DonBotAggregateDeliveryState::Ambiguous, None, None, None);
                } else if self.command.take().is_some() {
                    self.publish(
                        DonBotAggregateDeliveryState::Failed,
                        Some(DonBotAggregateDeliveryError::DispatchFailed),
                        None,
                        None,
                    );
                }
                return Ok(());
            }
        }
        if let Some(command) = self.command.take() {
            let request = match self.prepare(&command, uploads, configuration, donbot) {
                Ok(request) => request,
                Err(error) => {
                    self.publish(
                        DonBotAggregateDeliveryState::Failed,
                        Some(error),
                        None,
                        None,
                    );
                    return Ok(());
                }
            };
            let id = request.request_id;
            self.next_request += 1;
            match catch_unwind(AssertUnwindSafe(|| self.delivery.enqueue(request))) {
                Ok(Ok(())) => {
                    self.active_request = Some(id);
                    self.publish(DonBotAggregateDeliveryState::Sending, None, None, None);
                }
                Ok(Err(_)) => self.publish(
                    DonBotAggregateDeliveryState::Failed,
                    Some(DonBotAggregateDeliveryError::DispatchFailed),
                    None,
                    None,
                ),
                // Enqueue may have accepted the request before unwinding.
                Err(_) => self.publish(DonBotAggregateDeliveryState::Ambiguous, None, None, None),
            }
        }
        Ok(())
    }

    fn prepare<C: Clock>(
        &self,
        command: &SendDonBotAggregateCommand,
        uploads: &UploadCoordinator<C>,
        configuration: &ConfigurationService,
        donbot: &DonBotConfigurationSnapshot,
    ) -> Result<DonBotAggregateRequest, DonBotAggregateDeliveryError> {
        let current = configuration.snapshot();
        if command.configuration_revision != current.revision
            || command.donbot_revision != donbot.revision
        {
            return Err(DonBotAggregateDeliveryError::StaleConfiguration);
        }
        let route = authorized_donbot_aggregate_route(&current.settings, donbot);
        let guild = donbot
            .guilds
            .iter()
            .find(|g| g.guild_id == donbot.selected_guild_id)
            .ok_or(DonBotAggregateDeliveryError::Unavailable)?;
        if donbot.refreshing
            || !donbot.discord_aggregate_delivery_v1
            || !guild.discord_delivery.aggregate_enabled
            || !route.requested()
            || !(2..=MAX_DONBOT_AGGREGATE_FIGHTS)
                .contains(&guild.discord_delivery.max_aggregate_fight_logs)
            || command.job_ids.len() > usize::from(guild.discord_delivery.max_aggregate_fight_logs)
        {
            return Err(DonBotAggregateDeliveryError::Unavailable);
        }
        let jobs = uploads.snapshots();
        let mut unique = HashSet::with_capacity(command.job_ids.len());
        let mut fight_log_ids = Vec::with_capacity(command.job_ids.len());
        for id in &command.job_ids {
            let job = jobs
                .iter()
                .find(|job| job.id == *id)
                .ok_or(DonBotAggregateDeliveryError::UnknownJob)?;
            let fight = job
                .record
                .donbot_upload_receipt
                .as_ref()
                .and_then(|receipt| receipt.fight_log_id)
                .filter(|&id| id > 0 && id <= i64::MAX as u64 && unique.insert(id))
                .ok_or(DonBotAggregateDeliveryError::IneligibleJob)?;
            fight_log_ids.push(fight);
        }
        Ok(DonBotAggregateRequest {
            request_id: self.next_request,
            api_base_url: donbot.api_base_url.clone(),
            guild_id: current.settings.donbot.selected_guild_id,
            fight_log_ids,
            delivery: route,
        })
    }

    fn handle_result(&mut self, result: DonBotAggregateResult) {
        if self.active_request != Some(result.request_id) {
            return;
        }
        self.active_request = None;
        let valid_receipt = result.discord_delivery.as_ref().is_none_or(|receipt| {
            receipt.is_valid_with_limit(MAX_DONBOT_AGGREGATE_FIGHTS)
                && receipt.outcome != DonBotDiscordDeliveryOutcome::NotRequested
                && match result.outcome {
                    DonBotAggregateOutcome::Ambiguous => receipt.ambiguous > 0,
                    DonBotAggregateOutcome::Succeeded => receipt.sent > 0 && receipt.ambiguous == 0,
                    DonBotAggregateOutcome::Failed => receipt.sent == 0 && receipt.ambiguous == 0,
                    DonBotAggregateOutcome::Cancelled => false,
                }
        });
        if result.fight_log_count != self.snapshot.fight_log_count
            || !valid_receipt
            || (result.outcome == DonBotAggregateOutcome::Succeeded
                && result.discord_delivery.is_none())
            || (result.credential_failure.is_some()
                && (result.outcome != DonBotAggregateOutcome::Failed
                    || result.discord_delivery.is_some()))
        {
            self.publish(DonBotAggregateDeliveryState::Ambiguous, None, None, None);
            return;
        }
        let state = match result.outcome {
            DonBotAggregateOutcome::Succeeded => DonBotAggregateDeliveryState::Succeeded,
            DonBotAggregateOutcome::Failed => DonBotAggregateDeliveryState::Failed,
            DonBotAggregateOutcome::Ambiguous => DonBotAggregateDeliveryState::Ambiguous,
            DonBotAggregateOutcome::Cancelled => DonBotAggregateDeliveryState::Cancelled,
        };
        self.publish(
            state,
            None,
            result.discord_delivery,
            result.credential_failure,
        );
    }

    fn publish(
        &mut self,
        state: DonBotAggregateDeliveryState,
        diagnostic: Option<DonBotAggregateDeliveryError>,
        receipt: Option<DonBotDiscordDeliveryReceipt>,
        credential_failure: Option<UploadFailure>,
    ) {
        self.snapshot.state = state;
        self.snapshot.diagnostic = diagnostic;
        self.snapshot.discord_delivery = receipt;
        self.snapshot.credential_failure = credential_failure;
        self.snapshot.revision += 1;
    }
    pub fn shutdown(&mut self) {
        if self.snapshot.shutting_down {
            return;
        }
        self.snapshot.shutting_down = true;
        self.command = None;
        self.active_request = None;
        let _ = catch_unwind(AssertUnwindSafe(|| self.delivery.cancel_pending()));
        self.publish(DonBotAggregateDeliveryState::ShuttingDown, None, None, None);
    }
}
impl Drop for DonBotAggregateDeliveryController {
    fn drop(&mut self) {
        self.shutdown();
    }
}
