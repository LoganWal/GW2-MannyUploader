use super::*;
use crate::domain::{Settings, canonical_discord_id};
use std::{
    collections::VecDeque,
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex, MutexGuard},
};

#[derive(Debug)]
pub enum NexusOptionsCommand {
    SaveOrdinary(NexusOrdinaryOptions),
    SetWindowVisible(bool),
    SetDpsReportEnabled(bool),
    SetDpsReportDetailedWvw(bool),
    SetWingmanEnabled(bool),
    VerifyDonBot {
        api_base_url: String,
        api_key: SecretValue,
    },
    SelectDonBotGuild(String),
    SetDonBotEnabled(bool),
    RefreshDonBot,
    SetDonBotDiscordDeliveryEnabled(bool),
    SelectDonBotDiscordChannel(String),
    DisconnectDonBot,
    ConnectTwitch,
    OpenTwitchAuthorization {
        revision: u64,
    },
    SetTwitchEnabled(bool),
    DisconnectTwitch,
    SendTwitchTestMessage,
    DismissError,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NexusOptionsControllerConfig {
    pub command_capacity: usize,
    pub max_commands_per_tick: usize,
}
impl Default for NexusOptionsControllerConfig {
    fn default() -> Self {
        Self {
            command_capacity: 32,
            max_commands_per_tick: 8,
        }
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NexusOptionsTickReport {
    pub commands_processed: usize,
    pub action_failures: usize,
    pub donbot_progressed: bool,
    pub twitch_progressed: bool,
    pub twitch_test_message_progressed: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SavedConnectionsReport {
    pub donbot_started: bool,
    pub twitch_started: bool,
    pub donbot_error: Option<DonBotConfigurationError>,
    pub twitch_error: Option<TwitchAuthenticationControllerError>,
}
struct Queue {
    commands: VecDeque<NexusOptionsCommand>,
    snapshot: NexusOptionsSnapshot,
    capacity: usize,
}
/// The render boundary has no references to storage, workflow controllers, or worker ports.
#[derive(Clone)]
pub struct NexusOptionsHandle(Arc<Mutex<Queue>>);
impl NexusOptionsHandle {
    fn lock(&self) -> MutexGuard<'_, Queue> {
        self.0.lock().unwrap_or_else(|poison| poison.into_inner())
    }
    pub fn snapshot(&self) -> NexusOptionsSnapshot {
        self.lock().snapshot.clone()
    }
    pub fn submit(&self, command: NexusOptionsCommand) -> Result<(), NexusOptionsError> {
        let validation = validate_command(&command);
        let mut queue = self.lock();
        if !queue.snapshot.accepting_commands || queue.snapshot.shutting_down {
            return Err(NexusOptionsError::ShuttingDown);
        }
        if queue.snapshot.revision >= u64::MAX - 1 {
            return Err(NexusOptionsError::RevisionExhausted);
        }
        let error = validation.err().or_else(|| {
            (queue.commands.len() == queue.capacity).then_some(NexusOptionsError::QueueFull)
        });
        if let Some(error) = error {
            queue.snapshot.last_error = Some(error.clone());
            queue.snapshot.revision += 1;
            return Err(error);
        }
        queue.commands.push_back(command);
        queue.snapshot.pending_commands = queue.commands.len();
        queue.snapshot.revision += 1;
        Ok(())
    }
    /// Close callback admission immediately. The application owner then shuts down the controller.
    pub fn stop_accepting(&self) {
        let mut queue = self.lock();
        if !queue.snapshot.accepting_commands {
            return;
        }
        queue.snapshot.accepting_commands = false;
        queue.commands.clear();
        queue.snapshot.pending_commands = 0;
        queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
    }
}

pub struct NexusOptionsController {
    handle: NexusOptionsHandle,
    configuration: Arc<Mutex<ConfigurationService>>,
    donbot: DonBotConfigurationController,
    twitch: TwitchAuthenticationController,
    test_message: TwitchTestMessageController,
    launcher: Box<dyn ExternalActionLauncher>,
    max_commands_per_tick: usize,
    stopped: bool,
    startup_started: bool,
}
impl NexusOptionsController {
    pub fn new(
        configuration: Arc<Mutex<ConfigurationService>>,
        donbot: DonBotConfigurationController,
        twitch: TwitchAuthenticationController,
        test_message: TwitchTestMessageController,
        launcher: Box<dyn ExternalActionLauncher>,
        options: NexusOptionsControllerConfig,
    ) -> Result<Self, NexusOptionsError> {
        if !(1..=256).contains(&options.command_capacity)
            || !(1..=options.command_capacity).contains(&options.max_commands_per_tick)
        {
            return Err(NexusOptionsError::InvalidConfiguration);
        }
        let config_snapshot = configuration
            .lock()
            .map_err(|_| NexusOptionsError::Internal)?
            .snapshot();
        if config_snapshot.shutting_down
            || donbot.snapshot().shutting_down
            || twitch.is_shutting_down()
            || test_message.snapshot().state == TwitchTestMessageState::ShuttingDown
        {
            return Err(NexusOptionsError::ShuttingDown);
        }
        let mut commands = VecDeque::new();
        commands
            .try_reserve_exact(options.command_capacity)
            .map_err(|_| NexusOptionsError::InvalidConfiguration)?;
        let snapshot = NexusOptionsSnapshot {
            configuration: config_snapshot,
            donbot: donbot.snapshot(),
            twitch: twitch.snapshot(),
            twitch_test_message: test_message.snapshot(),
            last_error: None,
            pending_commands: 0,
            revision: 1,
            accepting_commands: true,
            shutting_down: false,
        };
        Ok(Self {
            handle: NexusOptionsHandle(Arc::new(Mutex::new(Queue {
                commands,
                snapshot,
                capacity: options.command_capacity,
            }))),
            configuration,
            donbot,
            twitch,
            test_message,
            launcher,
            max_commands_per_tick: options.max_commands_per_tick,
            stopped: false,
            startup_started: false,
        })
    }
    pub fn handle(&self) -> NexusOptionsHandle {
        self.handle.clone()
    }

    /// A single application owner executes commands and polls each workflow once per tick.
    pub fn tick(&mut self) -> Result<NexusOptionsTickReport, NexusOptionsError> {
        self.tick_with_settings_applier(|_| Ok(()))
    }

    /// Apply shared client identity before another command can start authentication or chat.
    /// Failure leaves later commands queued for a future tick. The callback runs without locks.
    pub fn tick_with_settings_applier(
        &mut self,
        mut apply: impl FnMut(&Settings) -> Result<(), NexusOptionsError>,
    ) -> Result<NexusOptionsTickReport, NexusOptionsError> {
        if self.stopped || !self.handle.lock().snapshot.accepting_commands {
            self.shutdown();
            return Err(NexusOptionsError::ShuttingDown);
        }
        self.publish(None, false);
        self.apply_settings(&mut apply)?;
        let budget = self
            .handle
            .lock()
            .commands
            .len()
            .min(self.max_commands_per_tick);
        let mut report = NexusOptionsTickReport::default();
        for _ in 0..budget {
            let command = {
                let mut queue = self.handle.lock();
                if !queue.snapshot.accepting_commands {
                    break;
                }
                let Some(command) = queue.commands.pop_front() else {
                    break;
                };
                queue.snapshot.pending_commands = queue.commands.len();
                queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
                command
            };
            let result = catch_unwind(AssertUnwindSafe(|| self.execute(command)))
                .unwrap_or(Err(NexusOptionsError::Internal));
            report.commands_processed += 1;
            report.action_failures += usize::from(result.is_err());
            self.publish(result.err(), true);
            self.apply_settings(&mut apply)?;
        }
        if !self.handle.lock().snapshot.accepting_commands {
            self.shutdown();
            return Ok(report);
        }
        let donbot = catch_unwind(AssertUnwindSafe(|| {
            let mut config = self
                .configuration
                .lock()
                .map_err(|_| NexusOptionsError::Internal)?;
            self.donbot
                .poll(&mut config)
                .map_err(NexusOptionsError::DonBot)
        }))
        .unwrap_or(Err(NexusOptionsError::Internal));
        let twitch = catch_unwind(AssertUnwindSafe(|| {
            self.twitch.tick().map_err(NexusOptionsError::Twitch)
        }))
        .unwrap_or(Err(NexusOptionsError::Internal));
        let test = self
            .test_message
            .poll()
            .map_err(NexusOptionsError::TestMessage);
        report.donbot_progressed = donbot.as_ref().copied().unwrap_or(false);
        report.twitch_progressed = twitch.as_ref().copied().unwrap_or(false);
        report.twitch_test_message_progressed = test.as_ref().copied().unwrap_or(false);
        let error = donbot.err().or_else(|| twitch.err()).or_else(|| test.err());
        self.publish(error, false);
        self.apply_settings(&mut apply)?;
        Ok(report)
    }
    fn apply_settings(
        &self,
        apply: &mut impl FnMut(&Settings) -> Result<(), NexusOptionsError>,
    ) -> Result<(), NexusOptionsError> {
        let settings = self.handle.snapshot().configuration.settings;
        let result = catch_unwind(AssertUnwindSafe(|| apply(&settings)))
            .unwrap_or(Err(NexusOptionsError::SettingsApplyFailed));
        if let Err(error) = &result {
            self.publish(Some(error.clone()), false);
        }
        result
    }

    pub fn start_saved_connections(
        &mut self,
        twitch_configured: bool,
    ) -> Result<SavedConnectionsReport, NexusOptionsError> {
        if self.stopped {
            return Err(NexusOptionsError::ShuttingDown);
        }
        if self.startup_started {
            return Ok(SavedConnectionsReport::default());
        }
        self.startup_started = true;
        let current = self
            .configuration
            .lock()
            .map_err(|_| NexusOptionsError::Internal)?
            .snapshot();
        let mut report = SavedConnectionsReport::default();
        if current.persistent_secret_storage.state == PersistentSecretStorageState::Available {
            if !current.settings.donbot.selected_guild_id.is_empty() {
                let result = catch_unwind(AssertUnwindSafe(|| {
                    let config = self
                        .configuration
                        .lock()
                        .map_err(|_| DonBotConfigurationError::SecretLoadFailed)?;
                    self.donbot.begin_saved_verification(&config)
                }))
                .unwrap_or(Err(DonBotConfigurationError::SecretLoadFailed));
                report.donbot_started = result.is_ok();
                report.donbot_error = result.err();
            }
            if twitch_configured {
                match catch_unwind(AssertUnwindSafe(|| self.twitch.begin_saved_connection()))
                    .unwrap_or(Err(TwitchAuthenticationControllerError::Internal))
                {
                    Ok(started) => report.twitch_started = started,
                    Err(error) => report.twitch_error = Some(error),
                }
            }
        }
        self.publish(
            report
                .donbot_error
                .map(NexusOptionsError::DonBot)
                .or_else(|| report.twitch_error.map(NexusOptionsError::Twitch)),
            false,
        );
        Ok(report)
    }

    pub fn refresh_donbot(&mut self) -> Result<(), NexusOptionsError> {
        if self.stopped {
            return Err(NexusOptionsError::ShuttingDown);
        }
        let result = catch_unwind(AssertUnwindSafe(|| {
            let config = self
                .configuration
                .lock()
                .map_err(|_| NexusOptionsError::Internal)?;
            self.donbot
                .begin_saved_refresh(&config)
                .map_err(NexusOptionsError::DonBot)
        }))
        .unwrap_or(Err(NexusOptionsError::Internal));
        self.publish(result.as_ref().err().cloned(), false);
        result
    }

    /// Runtime shutdown calls this after stopping command admission and provider work.
    pub fn shutdown_workflows(&mut self) {
        self.shutdown();
        self.donbot.shutdown();
        self.twitch.shutdown();
    }
    fn publish(&self, error: Option<NexusOptionsError>, clear_error: bool) {
        let configuration = self
            .configuration
            .lock()
            .ok()
            .map(|configuration| configuration.snapshot());
        let donbot = self.donbot.snapshot();
        let twitch = self.twitch.snapshot();
        let test = self.test_message.snapshot();
        let mut queue = self.handle.lock();
        if queue.snapshot.shutting_down {
            return;
        }
        let before = queue.snapshot.clone();
        // A panicking adapter poisons configuration access. Keep the last published settings
        // and expose the failure without reopening the poisoned service for mutations.
        let error = if let Some(configuration) = configuration {
            queue.snapshot.configuration = configuration;
            error
        } else {
            Some(NexusOptionsError::Internal)
        };
        queue.snapshot.donbot = donbot;
        queue.snapshot.twitch = twitch;
        queue.snapshot.twitch_test_message = test;
        if error.is_some() || clear_error {
            queue.snapshot.last_error = error;
        }
        if before != queue.snapshot {
            queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
        }
    }
    fn execute(&mut self, command: NexusOptionsCommand) -> Result<(), NexusOptionsError> {
        use NexusOptionsCommand as Command;
        // Twitch operations can lock the shared session owner and configuration themselves.
        // Keep the configuration guard scoped entirely inside the non-Twitch branches.
        match command {
            Command::ConnectTwitch => self
                .twitch
                .begin_connection()
                .map_err(NexusOptionsError::Twitch),
            Command::DisconnectTwitch => {
                self.twitch.disconnect().map_err(NexusOptionsError::Twitch)
            }
            Command::SendTwitchTestMessage => self
                .test_message
                .send(&self.twitch.snapshot())
                .map(|_| ())
                .map_err(NexusOptionsError::TestMessage),
            Command::OpenTwitchAuthorization { revision } => {
                let snapshot = self.twitch.snapshot();
                let uri = snapshot
                    .verification_uri
                    .as_deref()
                    .filter(|uri| {
                        snapshot.state == TwitchConnectionState::AwaitingUser
                            && !snapshot.shutting_down
                            && snapshot.revision == revision
                            && valid_twitch_authorization_uri(uri)
                    })
                    .ok_or(NexusOptionsError::AuthorizationUnavailable)?;
                catch_unwind(AssertUnwindSafe(|| self.launcher.open_url(uri)))
                    .map_err(|_| NexusOptionsError::BrowserLaunchFailed)?
                    .map_err(|_| NexusOptionsError::BrowserLaunchFailed)
            }
            Command::DismissError => Ok(()),
            command => {
                let mut config = self
                    .configuration
                    .lock()
                    .map_err(|_| NexusOptionsError::Internal)?;
                let mut settings = config.snapshot().settings;
                match command {
                    Command::SaveOrdinary(draft) => {
                        settings = draft.merge(&settings, self.twitch.snapshot().state)?
                    }
                    Command::SetWindowVisible(value) => settings.general.window_visible = value,
                    Command::SetDpsReportEnabled(value) => {
                        settings.dps_report.enabled = value;
                        if !value {
                            settings.twitch.enabled = false;
                        }
                    }
                    Command::SetDpsReportDetailedWvw(value) => {
                        settings.dps_report.detailed_wvw = value
                    }
                    Command::SetWingmanEnabled(value) => settings.wingman.enabled = value,
                    Command::SetTwitchEnabled(value) => {
                        if value && self.twitch.snapshot().state != TwitchConnectionState::Connected
                        {
                            return Err(NexusOptionsError::Twitch(
                                TwitchAuthenticationControllerError::InvalidState,
                            ));
                        }
                        settings.twitch.enabled = value;
                    }
                    Command::VerifyDonBot {
                        api_base_url,
                        api_key,
                    } => {
                        return self
                            .donbot
                            .begin_verification(&config, api_base_url, api_key)
                            .map_err(NexusOptionsError::DonBot);
                    }
                    Command::SelectDonBotGuild(id) => {
                        return self
                            .donbot
                            .select_guild(&mut config, id)
                            .map_err(NexusOptionsError::DonBot);
                    }
                    Command::RefreshDonBot => {
                        return self
                            .donbot
                            .begin_saved_refresh(&config)
                            .map_err(NexusOptionsError::DonBot);
                    }
                    Command::SetDonBotDiscordDeliveryEnabled(value) => {
                        return self
                            .donbot
                            .set_discord_delivery_enabled(&mut config, value)
                            .map_err(NexusOptionsError::DonBot);
                    }
                    Command::SelectDonBotDiscordChannel(id) => {
                        return self
                            .donbot
                            .select_discord_channel(&mut config, id)
                            .map_err(NexusOptionsError::DonBot);
                    }
                    Command::DisconnectDonBot => {
                        return self
                            .donbot
                            .disconnect(&mut config)
                            .map_err(NexusOptionsError::DonBot);
                    }
                    Command::SetDonBotEnabled(value) => {
                        let bot = self.donbot.snapshot();
                        if bot.refreshing || bot.state == DonBotConfigurationState::Verifying {
                            return Err(NexusOptionsError::DonBot(DonBotConfigurationError::Busy));
                        }
                        if bot.shutting_down {
                            return Err(NexusOptionsError::ShuttingDown);
                        }
                        if value
                            && (bot.state != DonBotConfigurationState::Verified
                                || bot.account_name.is_none()
                                || normalize_donbot_api_base(&settings.donbot.api_base_url)
                                    .as_deref()
                                    != Some(bot.api_base_url.as_str())
                                || settings.donbot.selected_guild_id != bot.selected_guild_id
                                || !bot
                                    .guilds
                                    .iter()
                                    .any(|guild| guild.guild_id == bot.selected_guild_id))
                        {
                            return Err(NexusOptionsError::DonBot(
                                DonBotConfigurationError::NotVerified,
                            ));
                        }
                        settings.donbot.enabled = value;
                        if !value {
                            settings.donbot.discord_delivery_enabled = false;
                            settings.donbot.discord_channel_override_explicit = false;
                            settings.donbot.selected_discord_channel_id.clear();
                        }
                        config
                            .save_settings(settings)
                            .map_err(NexusOptionsError::Configuration)?;
                        return if value {
                            self.donbot
                                .begin_saved_refresh(&config)
                                .map_err(NexusOptionsError::DonBot)
                        } else {
                            Ok(())
                        };
                    }
                    _ => unreachable!("Twitch commands are handled without a configuration guard"),
                }
                config
                    .save_settings(settings)
                    .map_err(NexusOptionsError::Configuration)
            }
        }
    }
    pub fn shutdown(&mut self) {
        if self.stopped {
            return;
        }
        self.stopped = true;
        self.handle.stop_accepting();
        self.test_message.shutdown();
        let mut queue = self.handle.lock();
        queue.snapshot.twitch_test_message = self.test_message.snapshot();
        queue.snapshot.shutting_down = true;
        queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
    }
}
impl Drop for NexusOptionsController {
    fn drop(&mut self) {
        self.shutdown();
    }
}
fn validate_command(command: &NexusOptionsCommand) -> Result<(), NexusOptionsError> {
    use NexusOptionsCommand as Command;
    let valid = match command {
        Command::SaveOrdinary(draft) => {
            draft.merge(
                &Settings::new("validation".into()),
                TwitchConnectionState::Disconnected,
            )?;
            true
        }
        Command::VerifyDonBot {
            api_base_url,
            api_key,
        } => normalize_donbot_api_base(api_base_url).is_some() && valid_donbot_api_key(api_key),
        Command::SelectDonBotGuild(id) => canonical_discord_id(id),
        Command::SelectDonBotDiscordChannel(id) => id.is_empty() || canonical_discord_id(id),
        Command::OpenTwitchAuthorization { revision } => *revision != 0,
        _ => true,
    };
    if valid {
        Ok(())
    } else {
        Err(NexusOptionsError::InvalidCommand)
    }
}
pub fn valid_twitch_authorization_uri(uri: &str) -> bool {
    valid_external_url(uri)
        && !uri.contains(['@', '#'])
        && uri
            .strip_prefix("https://www.twitch.tv/activate")
            .is_some_and(|suffix| suffix.is_empty() || suffix.starts_with('?'))
}
