use super::*;
use crate::domain::*;
use std::{
    collections::HashSet,
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex, MutexGuard},
    time::{Duration, Instant},
};

const DONBOT_REFRESH: Duration = Duration::from_secs(300);

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RuntimeError {
    InvalidConfiguration,
    Internal,
    ShuttingDown,
    TimeOutOfRange,
    ThreadStartFailed,
    Options(NexusOptionsError),
    Pump(PumpError),
    History(HistoryStoreError),
    Provider(CoordinatorError),
    Parser(IngestionError),
    Selection(SelectionError),
    Actions(RecentLogActionError),
    Aggregate(DonBotAggregateDeliveryError),
}
impl RuntimeError {
    pub fn detail(&self) -> &'static str {
        match self {
            Self::InvalidConfiguration => "The addon runtime is configured incorrectly",
            Self::Internal => "The application owner encountered an unexpected failure",
            Self::ShuttingDown => "The addon is shutting down",
            Self::TimeOutOfRange => "The application clock is outside its supported range",
            Self::ThreadStartFailed => "The application owner thread could not be started",
            Self::Options(error) => error.detail(),
            Self::Pump(_) => "Log discovery or processing could not be completed",
            Self::History(_) => "Upload history could not be saved",
            Self::Provider(_) => "Provider settings could not be applied",
            Self::Parser(_) => "Parser settings could not be applied",
            Self::Selection(_) => "The log selection could not be applied",
            Self::Actions(error) => error.detail(),
            Self::Aggregate(error) => error.detail(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RuntimeSnapshot {
    pub options: NexusOptionsSnapshot,
    pub jobs: Vec<UploadJobSnapshot>,
    pub recent_actions: RecentLogActionsSnapshot,
    pub aggregate: DonBotAggregateDeliverySnapshot,
    pub selection: SessionLogSelection,
    pub root_available: bool,
    pub source_issues: Vec<CandidateIssue>,
    pub history_recovery: Option<HistoryRecovery>,
    pub diagnostic: Option<RuntimeError>,
    pub revision: u64,
    pub shutting_down: bool,
}
struct Published {
    snapshot: RuntimeSnapshot,
    requested_selection: LogSelectionMode,
    aggregate_command: Option<SendDonBotAggregateCommand>,
    accepting: bool,
}
#[derive(Clone)]
pub struct RuntimeHandle {
    state: Arc<Mutex<Published>>,
    options: NexusOptionsHandle,
    actions: Arc<RecentLogActionsController>,
}
impl RuntimeHandle {
    fn lock(&self) -> MutexGuard<'_, Published> {
        self.state.lock().unwrap_or_else(|error| error.into_inner())
    }
    pub fn snapshot(&self) -> RuntimeSnapshot {
        self.lock().snapshot.clone()
    }
    pub fn submit_options(&self, command: NexusOptionsCommand) -> Result<(), NexusOptionsError> {
        self.options.submit(command)
    }
    pub fn submit_action(&self, command: RecentLogAction) -> Result<(), RecentLogActionError> {
        self.actions.submit(command)
    }
    pub fn select_logs(&self, selection: LogSelectionMode) -> Result<(), RuntimeError> {
        let mut state = self.lock();
        if !state.accepting {
            return Err(RuntimeError::ShuttingDown);
        }
        state.requested_selection = selection;
        Ok(())
    }
    pub fn submit_aggregate(
        &self,
        command: SendDonBotAggregateCommand,
    ) -> Result<(), DonBotAggregateDeliveryError> {
        if command.configuration_revision == 0
            || command.donbot_revision == 0
            || !(2..=usize::from(MAX_DONBOT_AGGREGATE_FIGHTS)).contains(&command.job_ids.len())
            || command
                .job_ids
                .iter()
                .copied()
                .collect::<HashSet<_>>()
                .len()
                != command.job_ids.len()
        {
            return Err(DonBotAggregateDeliveryError::InvalidCommand);
        }
        let mut state = self.lock();
        if !state.accepting {
            return Err(DonBotAggregateDeliveryError::ShuttingDown);
        }
        if state.aggregate_command.is_some()
            || matches!(
                state.snapshot.aggregate.state,
                DonBotAggregateDeliveryState::Queued | DonBotAggregateDeliveryState::Sending
            )
        {
            return Err(DonBotAggregateDeliveryError::Busy);
        }
        state.snapshot.aggregate.state = DonBotAggregateDeliveryState::Queued;
        state.snapshot.aggregate.fight_log_count = command.job_ids.len();
        state.snapshot.revision = state.snapshot.revision.saturating_add(1);
        state.aggregate_command = Some(command);
        Ok(())
    }
    pub fn close_admission(&self) {
        self.options.stop_accepting();
        self.actions.shutdown();
        let mut state = self.lock();
        state.accepting = false;
        state.aggregate_command = None;
    }
}

/// Construct these dependencies outside rendering, then transfer them to one application owner.
pub struct RuntimeParts<C: Clock> {
    pub pump: ApplicationPump<C>,
    pub options: NexusOptionsController,
    pub aggregate: DonBotAggregateDeliveryController,
    pub history: Box<dyn UploadHistoryStore>,
    pub configuration: Arc<Mutex<ConfigurationService>>,
    pub twitch_application: Arc<dyn TwitchApplicationConfiguration>,
    pub launcher: Box<dyn ExternalActionLauncher>,
    pub clock: Arc<dyn Clock + Send + Sync>,
}
#[derive(Default)]
struct Applied {
    source: Option<CandidateSourceOptions>,
    stability: Option<usize>,
    parser_capacity: Option<usize>,
    history_limit: Option<usize>,
    policies: [Option<ProviderConfiguration>; 4],
    client_id: Option<String>,
    poll_interval: Option<Duration>,
}
pub struct ApplicationRuntime<C: Clock> {
    parts: RuntimeParts<C>,
    handle: RuntimeHandle,
    applied: Applied,
    bundled_client_id: String,
    selection: SessionLogSelection,
    next_poll: Instant,
    next_refresh: Instant,
    refreshing: bool,
    last_persisted: Vec<UploadJobRecord>,
    root_available: bool,
    source_issues: Vec<CandidateIssue>,
    source_error: Option<PumpError>,
    stopped: bool,
}
impl<C: Clock> ApplicationRuntime<C> {
    pub fn new(parts: RuntimeParts<C>, bundled_client_id: String) -> Result<Self, RuntimeError> {
        catch_unwind(AssertUnwindSafe(|| {
            Self::initialize(parts, bundled_client_id)
        }))
        .unwrap_or(Err(RuntimeError::Internal))
    }
    fn initialize(
        mut parts: RuntimeParts<C>,
        bundled_client_id: String,
    ) -> Result<Self, RuntimeError> {
        if !bundled_client_id.is_empty() && !valid_twitch_client_id(&bundled_client_id) {
            return Err(RuntimeError::InvalidConfiguration);
        }
        let initial = parts
            .configuration
            .lock()
            .map_err(|_| RuntimeError::Internal)?
            .snapshot();
        if initial.shutting_down || !initial.settings.validate().is_empty() {
            return Err(RuntimeError::InvalidConfiguration);
        }
        let steady = parts.clock.steady_now();
        let selection = SessionLogSelection::new(parts.clock.system_now(), steady);
        let persisted = parts.history.records().to_vec();
        parts
            .pump
            .restore_history(&persisted)
            .map_err(RuntimeError::Pump)?;
        let actions = Arc::new(
            RecentLogActionsController::new(Default::default()).map_err(RuntimeError::Actions)?,
        );
        let options = parts.options.handle();
        let snapshot = RuntimeSnapshot {
            options: options.snapshot(),
            jobs: parts.pump.ingestion().uploads().snapshots(),
            recent_actions: actions.snapshot(),
            aggregate: parts.aggregate.snapshot(),
            selection,
            root_available: false,
            source_issues: vec![],
            history_recovery: parts.history.recovery(),
            diagnostic: None,
            revision: 1,
            shutting_down: false,
        };
        let mut runtime = Self {
            parts,
            handle: RuntimeHandle {
                state: Arc::new(Mutex::new(Published {
                    snapshot,
                    requested_selection: LogSelectionMode::New,
                    aggregate_command: None,
                    accepting: true,
                })),
                options,
                actions,
            },
            applied: Applied::default(),
            bundled_client_id,
            selection,
            next_poll: steady,
            next_refresh: steady
                .checked_add(DONBOT_REFRESH)
                .ok_or(RuntimeError::TimeOutOfRange)?,
            refreshing: false,
            last_persisted: persisted,
            root_available: false,
            source_issues: vec![],
            source_error: None,
            stopped: false,
        };
        runtime.apply_client_identity(&initial.settings)?;
        runtime.apply_configuration(&runtime.handle.options.snapshot())?;
        let configured = runtime
            .applied
            .client_id
            .as_ref()
            .is_some_and(|id| !id.is_empty());
        let startup = runtime
            .parts
            .options
            .start_saved_connections(configured)
            .map_err(RuntimeError::Options)?;
        let error = startup
            .donbot_error
            .map(NexusOptionsError::DonBot)
            .or_else(|| startup.twitch_error.map(NexusOptionsError::Twitch))
            .map(RuntimeError::Options);
        runtime.publish(error);
        Ok(runtime)
    }
    pub fn handle(&self) -> RuntimeHandle {
        self.handle.clone()
    }
    pub fn tick(&mut self, stop: &StopToken) -> Result<(), RuntimeError> {
        if self.stopped {
            return Err(RuntimeError::ShuttingDown);
        }
        if stop.is_stop_requested() || !self.handle.lock().accepting {
            self.shutdown();
            return Err(RuntimeError::ShuttingDown);
        }
        let result = catch_unwind(AssertUnwindSafe(|| self.tick_inner(stop)))
            .unwrap_or(Err(RuntimeError::Internal));
        self.publish(result.as_ref().err().cloned());
        result
    }
    fn tick_inner(&mut self, stop: &StopToken) -> Result<(), RuntimeError> {
        let client = self.parts.twitch_application.clone();
        let bundled = &self.bundled_client_id;
        let applied = &mut self.applied.client_id;
        let options_tick = self
            .parts
            .options
            .tick_with_settings_applier(|settings| {
                apply_client(&*client, bundled, applied, settings)
            })
            .map_err(RuntimeError::Options)?;
        let mut options = self.handle.options.snapshot();
        self.apply_configuration(&options)?;
        let now = self.parts.clock.steady_now();
        if (self.refreshing || options_tick.donbot_progressed) && !options.donbot.refreshing {
            self.next_refresh = now
                .checked_add(DONBOT_REFRESH)
                .ok_or(RuntimeError::TimeOutOfRange)?;
        }
        self.refreshing = options.donbot.refreshing;
        if options.configuration.settings.donbot.enabled
            && options.donbot.state == DonBotConfigurationState::Verified
            && !options.donbot.refreshing
            && now >= self.next_refresh
        {
            self.next_refresh = now
                .checked_add(DONBOT_REFRESH)
                .ok_or(RuntimeError::TimeOutOfRange)?;
            self.parts
                .options
                .refresh_donbot()
                .map_err(RuntimeError::Options)?;
            options = self.handle.options.snapshot();
            self.refreshing = options.donbot.refreshing;
        }
        // Retry a failed accepted-identity write before allowing parser results to dispatch uploads.
        self.persist_history()?;
        self.handle
            .actions
            .tick(
                self.parts.pump.ingestion_mut().uploads_mut(),
                &mut *self.parts.launcher,
            )
            .map_err(RuntimeError::Actions)?;
        let aggregate = self.handle.lock().aggregate_command.take();
        if let Some(command) = aggregate {
            self.parts
                .aggregate
                .submit(command)
                .map_err(RuntimeError::Aggregate)?;
        }
        {
            let configuration = self
                .parts
                .configuration
                .lock()
                .map_err(|_| RuntimeError::Internal)?;
            self.parts
                .aggregate
                .tick(
                    self.parts.pump.ingestion().uploads(),
                    &configuration,
                    &options.donbot,
                )
                .map_err(RuntimeError::Aggregate)?;
        }
        let scan = now >= self.next_poll;
        let result = if scan {
            self.next_poll = now
                .checked_add(self.applied.poll_interval.unwrap_or(Duration::from_secs(1)))
                .ok_or(RuntimeError::TimeOutOfRange)?;
            self.parts
                .pump
                .tick(options.configuration.settings.enabled_providers(), stop)
        } else {
            self.parts.pump.drain_work(stop)
        };
        if scan {
            self.source_error = result.as_ref().err().copied();
            if let Ok(report) = &result {
                self.root_available = report.root_available;
                self.source_issues = report.issues.clone();
            }
        }
        // A source failure may follow successful result drainage, so retain those receipts too.
        self.persist_history()?;
        result.map_err(RuntimeError::Pump)?;
        self.source_error
            .map_or(Ok(()), |error| Err(RuntimeError::Pump(error)))
    }
    fn apply_client_identity(&mut self, settings: &Settings) -> Result<(), RuntimeError> {
        apply_client(
            &*self.parts.twitch_application,
            &self.bundled_client_id,
            &mut self.applied.client_id,
            settings,
        )
        .map_err(RuntimeError::Options)
    }
    fn apply_configuration(&mut self, options: &NexusOptionsSnapshot) -> Result<(), RuntimeError> {
        let settings = &options.configuration.settings;
        let general = &settings.general;
        let now = self.parts.clock.steady_now();
        let desired = self.handle.lock().requested_selection;
        let proposal = self
            .selection
            .propose(desired, self.parts.clock.system_now(), now)
            .map_err(RuntimeError::Selection)?;
        let selection = proposal.unwrap_or(self.selection);
        let source = CandidateSourceOptions {
            root: general.log_directory.clone().into(),
            recursive: general.watch_subdirectories,
            max_candidates: general.max_candidates as usize,
            minimum_last_write_time: Some(selection.discovery_cutoff()),
        };
        if self.applied.source.as_ref() != Some(&source) {
            self.parts
                .pump
                .reconfigure_source(source.clone())
                .map_err(RuntimeError::Pump)?;
            self.applied.source = Some(source);
            self.next_poll = now;
            self.root_available = false;
        }
        self.selection = selection;
        let stability = general.stability_observations as usize;
        if self.applied.stability != Some(stability) {
            self.parts
                .pump
                .set_required_matches(stability)
                .map_err(RuntimeError::Pump)?;
            self.applied.stability = Some(stability);
            self.next_poll = now;
        }
        let capacity = general.parser_queue_capacity as usize;
        if self.applied.parser_capacity != Some(capacity) {
            self.parts
                .pump
                .ingestion_mut()
                .update_parser_capacity(capacity)
                .map_err(RuntimeError::Parser)?;
            self.applied.parser_capacity = Some(capacity);
        }
        let limit = general.recent_log_limit as usize;
        if self.applied.history_limit != Some(limit) {
            self.parts
                .pump
                .ingestion_mut()
                .uploads_mut()
                .set_history_limit(limit)
                .map_err(RuntimeError::Provider)?;
            self.applied.history_limit = Some(limit);
        }
        let policies = [
            ProviderPolicy::DpsReport {
                detailed_wvw: settings.dps_report.detailed_wvw,
            },
            ProviderPolicy::Wingman,
            ProviderPolicy::DonBot {
                api_base_url: settings.donbot.api_base_url.clone(),
                guild_id: settings.donbot.selected_guild_id.clone(),
                delivery: authorized_donbot_delivery(settings, &options.donbot),
            },
            ProviderPolicy::Twitch {
                message_template: settings.twitch.message_template.clone(),
                post_success: settings.twitch.post_success,
                post_failure: settings.twitch.post_failure,
            },
        ];
        for (index, policy) in policies.into_iter().enumerate() {
            let configuration = ProviderConfiguration {
                parallelism: general.parallel_uploads_per_provider as usize,
                policy,
            };
            if self.applied.policies[index].as_ref() != Some(&configuration) {
                self.parts
                    .pump
                    .ingestion_mut()
                    .uploads_mut()
                    .configure_provider(configuration.clone())
                    .map_err(RuntimeError::Provider)?;
                self.applied.policies[index] = Some(configuration);
            }
        }
        let interval = Duration::from_millis(u64::from(general.poll_interval_ms));
        if self.applied.poll_interval != Some(interval) {
            self.next_poll = now;
            self.applied.poll_interval = Some(interval);
        }
        Ok(())
    }
    fn persist_history(&mut self) -> Result<(), RuntimeError> {
        let records = self.parts.pump.ingestion().uploads().history();
        if records != self.last_persisted {
            self.parts
                .history
                .merge_and_save(&records)
                .map_err(RuntimeError::History)?;
            self.last_persisted = records;
        }
        Ok(())
    }
    fn publish(&self, diagnostic: Option<RuntimeError>) {
        let options = self.handle.options.snapshot();
        let jobs = self.parts.pump.ingestion().uploads().snapshots();
        let actions = self.handle.actions.snapshot();
        let mut aggregate = self.parts.aggregate.snapshot();
        let mut state = self.handle.lock();
        if let Some(command) = &state.aggregate_command {
            aggregate.state = DonBotAggregateDeliveryState::Queued;
            aggregate.fight_log_count = command.job_ids.len();
        }
        let mut snapshot = RuntimeSnapshot {
            options,
            jobs,
            recent_actions: actions,
            aggregate,
            selection: self.selection,
            root_available: self.root_available,
            source_issues: self.source_issues.clone(),
            history_recovery: state.snapshot.history_recovery,
            diagnostic,
            revision: state.snapshot.revision,
            shutting_down: self.stopped,
        };
        if snapshot != state.snapshot {
            snapshot.revision = snapshot.revision.saturating_add(1);
            state.snapshot = snapshot;
        }
    }
    pub fn shutdown(&mut self) {
        if self.stopped {
            return;
        }
        self.stopped = true;
        self.handle.close_admission();
        self.parts.options.shutdown();
        self.parts.aggregate.shutdown();
        self.parts.pump.cancel_all();
        let error = catch_unwind(AssertUnwindSafe(|| self.persist_history()))
            .unwrap_or(Err(RuntimeError::Internal))
            .err();
        self.parts.options.shutdown_workflows();
        if let Ok(mut configuration) = self.parts.configuration.lock() {
            configuration.shutdown();
        }
        self.publish(error);
    }
}
impl<C: Clock> Drop for ApplicationRuntime<C> {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Owns the application thread and joins it, including destruction of its owned workers.
pub struct ApplicationRuntimeThread {
    stop: CancellationSource,
    handle: RuntimeHandle,
    thread: Option<std::thread::JoinHandle<()>>,
}
impl ApplicationRuntimeThread {
    pub fn start<C: Clock + Send + 'static>(
        mut runtime: ApplicationRuntime<C>,
    ) -> Result<Self, RuntimeError> {
        let stop = CancellationSource::default();
        let token = stop.token();
        let handle = runtime.handle();
        let thread = std::thread::Builder::new()
            .name("manny-application".into())
            .spawn(move || {
                while !token.is_stop_requested() && !runtime.stopped {
                    let _ = runtime.tick(&token);
                    if token.wait_for_stop(Duration::from_millis(50)) {
                        break;
                    }
                }
                runtime.shutdown();
            })
            .map_err(|_| RuntimeError::ThreadStartFailed)?;
        Ok(Self {
            stop,
            handle,
            thread: Some(thread),
        })
    }
    pub fn handle(&self) -> RuntimeHandle {
        self.handle.clone()
    }
    pub fn shutdown(&mut self) {
        self.handle.close_admission();
        self.stop.request_stop();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
impl Drop for ApplicationRuntimeThread {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn apply_client(
    client: &dyn TwitchApplicationConfiguration,
    bundled: &str,
    applied: &mut Option<String>,
    settings: &Settings,
) -> Result<(), NexusOptionsError> {
    let desired = if settings.twitch.client_id.is_empty() {
        bundled
    } else {
        &settings.twitch.client_id
    };
    if applied.as_deref() != Some(desired) {
        client
            .set_client_id(desired.to_owned())
            .map_err(|_| NexusOptionsError::SettingsApplyFailed)?;
        *applied = Some(desired.to_owned());
    }
    Ok(())
}
