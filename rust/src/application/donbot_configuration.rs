use std::panic::{AssertUnwindSafe, catch_unwind};

use super::{
    ConfigurationService, DonBotDeliveryRoute, DonBotDiscordDeliveryPolicy, DonBotGuild,
    DonBotVerification, DonBotVerificationFailure, DonBotVerificationRequest, DonBotVerifier,
    SecretId, SecretValue, normalize_donbot_api_base, valid_donbot_api_key,
};
use crate::domain::{DonBotSettings, Settings};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotConfigurationState {
    Unverified,
    Verifying,
    Verified,
    Error,
    ShuttingDown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotConfigurationError {
    InvalidConfiguration,
    Busy,
    DispatchFailed,
    VerificationFailed,
    VerificationCancelled,
    StaleVerification,
    SettingsSaveFailed,
    SecretLoadFailed,
    SecretStoreFailed,
    SecretEraseFailed,
    NotVerified,
    UnknownGuild,
    DeliveryUnavailable,
    UnknownChannel,
    ShuttingDown,
    RevisionExhausted,
}
impl DonBotConfigurationError {
    pub fn detail(self) -> &'static str {
        match self {
            Self::InvalidConfiguration => "The DonBot endpoint or API key is invalid",
            Self::Busy => "DonBot verification is already in progress",
            Self::DispatchFailed => "DonBot verification could not be started",
            Self::VerificationFailed => "DonBot verification failed",
            Self::VerificationCancelled => "DonBot verification was cancelled",
            Self::StaleVerification => "DonBot settings changed. Verify again",
            Self::SettingsSaveFailed => "DonBot settings could not be saved",
            Self::SecretLoadFailed => "The saved DonBot API key could not be loaded",
            Self::SecretStoreFailed => "The verified DonBot API key could not be saved",
            Self::SecretEraseFailed => "The saved DonBot API key could not be removed",
            Self::NotVerified => "Verify DonBot before changing server settings",
            Self::UnknownGuild => "The selected DonBot server is unavailable",
            Self::DeliveryUnavailable => "DonBot Discord delivery is unavailable",
            Self::UnknownChannel => "The selected DonBot Discord channel is unavailable",
            Self::ShuttingDown => "DonBot configuration is shutting down",
            Self::RevisionExhausted => "DonBot configuration cannot accept more changes",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotConfigurationSnapshot {
    pub state: DonBotConfigurationState,
    pub api_base_url: String,
    pub account_name: Option<String>,
    pub discord_summary_delivery_v1: bool,
    pub discord_aggregate_delivery_v1: bool,
    pub guilds: Vec<DonBotGuild>,
    pub selected_guild_id: String,
    pub diagnostic: Option<DonBotConfigurationError>,
    pub refreshing: bool,
    pub revision: u64,
    pub shutting_down: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Source {
    Candidate,
    Saved,
    Refresh,
}
struct Pending {
    id: u64,
    base: String,
    configured_base: String,
    source: Source,
}

/// Called only by the application owner, outside render callbacks. Storage remains shared by
/// borrowing the configuration service per command, without introducing another mutable owner.
pub struct DonBotConfigurationController {
    verifier: Box<dyn DonBotVerifier>,
    snapshot: DonBotConfigurationSnapshot,
    pending: Option<Pending>,
    next_request: u64,
}
impl DonBotConfigurationController {
    pub fn new(
        configuration: &ConfigurationService,
        verifier: Box<dyn DonBotVerifier>,
    ) -> Result<Self, DonBotConfigurationError> {
        if configuration.is_shutting_down() {
            return Err(DonBotConfigurationError::ShuttingDown);
        }
        let api_base_url =
            normalize_donbot_api_base(&configuration.snapshot().settings.donbot.api_base_url)
                .ok_or(DonBotConfigurationError::InvalidConfiguration)?;
        Ok(Self {
            verifier,
            snapshot: DonBotConfigurationSnapshot {
                state: DonBotConfigurationState::Unverified,
                api_base_url,
                account_name: None,
                discord_summary_delivery_v1: false,
                discord_aggregate_delivery_v1: false,
                guilds: vec![],
                selected_guild_id: String::new(),
                diagnostic: None,
                refreshing: false,
                revision: 1,
                shutting_down: false,
            },
            pending: None,
            next_request: 1,
        })
    }

    pub fn snapshot(&self) -> DonBotConfigurationSnapshot {
        self.snapshot.clone()
    }

    pub fn begin_verification(
        &mut self,
        configuration: &ConfigurationService,
        base: String,
        key: SecretValue,
    ) -> Result<(), DonBotConfigurationError> {
        self.dispatch(configuration, base, key, Source::Candidate)
    }

    pub fn begin_saved_verification(
        &mut self,
        configuration: &ConfigurationService,
    ) -> Result<(), DonBotConfigurationError> {
        self.begin_saved(configuration, Source::Saved)
    }

    pub fn begin_saved_refresh(
        &mut self,
        configuration: &ConfigurationService,
    ) -> Result<(), DonBotConfigurationError> {
        self.begin_saved(configuration, Source::Refresh)
    }

    fn begin_saved(
        &mut self,
        configuration: &ConfigurationService,
        source: Source,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_idle(configuration)?;
        if source == Source::Refresh {
            // A refresh retains visible authorization, so it must stay bound to its verified base.
            self.require_verified(configuration)?;
        }
        let key = guarded(|| configuration.load_secret(SecretId::DonBotGw2ApiKey))
            .map_err(|()| self.fail(DonBotConfigurationError::SecretLoadFailed, source))?;
        self.dispatch(
            configuration,
            configuration.snapshot().settings.donbot.api_base_url,
            key,
            source,
        )
    }

    fn dispatch(
        &mut self,
        configuration: &ConfigurationService,
        base: String,
        key: SecretValue,
        source: Source,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_idle(configuration)?;
        let base = normalize_donbot_api_base(&base)
            .filter(|_| valid_donbot_api_key(&key))
            .ok_or_else(|| self.fail(DonBotConfigurationError::InvalidConfiguration, source))?;
        let next = self
            .next_request
            .checked_add(1)
            .ok_or(DonBotConfigurationError::RevisionExhausted)?;
        let pending = Pending {
            id: self.next_request,
            base: base.clone(),
            configured_base: configuration.snapshot().settings.donbot.api_base_url,
            source,
        };
        self.next_request = next;
        guarded(|| {
            self.verifier.enqueue(DonBotVerificationRequest {
                request_id: pending.id,
                api_base_url: base.clone(),
                api_key: key,
            })
        })
        .map_err(|()| self.fail(DonBotConfigurationError::DispatchFailed, source))?;
        self.pending = Some(pending);
        if source == Source::Refresh {
            self.snapshot.refreshing = true;
        } else {
            self.clear_identity();
            self.snapshot.state = DonBotConfigurationState::Verifying;
        }
        self.snapshot.api_base_url = base;
        self.snapshot.diagnostic = None;
        self.advance();
        Ok(())
    }

    /// Consumes at most one result per application tick, including stale results.
    pub fn poll(
        &mut self,
        configuration: &mut ConfigurationService,
    ) -> Result<bool, DonBotConfigurationError> {
        self.require_running(configuration)?;
        let result = match catch_unwind(AssertUnwindSafe(|| self.verifier.try_take_result())) {
            Ok(Some(result)) => result,
            Ok(None) => return Ok(false),
            Err(_) => {
                let source = self.pending.take().map_or(Source::Saved, |p| p.source);
                return Err(self.fail(DonBotConfigurationError::VerificationFailed, source));
            }
        };
        if !self
            .pending
            .as_ref()
            .is_some_and(|p| p.id == result.request_id)
        {
            return Err(DonBotConfigurationError::StaleVerification);
        }
        let pending = self.pending.take().expect("matching request");
        let success = result.verification.map_err(|failure| {
            self.fail(
                match failure {
                    DonBotVerificationFailure::Failed => {
                        DonBotConfigurationError::VerificationFailed
                    }
                    DonBotVerificationFailure::Cancelled => {
                        DonBotConfigurationError::VerificationCancelled
                    }
                },
                pending.source,
            )
        })?;
        let mut settings = configuration.snapshot().settings;
        if normalize_donbot_api_base(&settings.donbot.api_base_url)
            != normalize_donbot_api_base(&pending.configured_base)
        {
            return Err(self.fail(DonBotConfigurationError::StaleVerification, pending.source));
        }
        if pending.source == Source::Candidate {
            disable(&mut settings.donbot);
            settings.donbot.api_base_url = pending.base.clone();
            self.save(configuration, settings, pending.source)?;
            guarded(|| configuration.store_secret(SecretId::DonBotGw2ApiKey, &success.api_key))
                .map_err(|()| {
                    self.fail(DonBotConfigurationError::SecretStoreFailed, pending.source)
                })?;
        } else {
            let previous = settings.clone();
            sanitize(&mut settings.donbot, &success.identity);
            if settings != previous {
                self.save(configuration, settings, pending.source)?;
            }
        }
        let identity = success.identity;
        self.snapshot.state = DonBotConfigurationState::Verified;
        self.snapshot.api_base_url = pending.base;
        self.snapshot.account_name = Some(identity.account_name);
        self.snapshot.guilds = identity.guilds;
        self.snapshot.discord_summary_delivery_v1 = identity.discord_summary_delivery_v1;
        self.snapshot.discord_aggregate_delivery_v1 = identity.discord_aggregate_delivery_v1;
        self.snapshot.selected_guild_id =
            configuration.snapshot().settings.donbot.selected_guild_id;
        self.snapshot.refreshing = false;
        self.snapshot.diagnostic = None;
        self.advance();
        Ok(true)
    }

    pub fn select_guild(
        &mut self,
        configuration: &mut ConfigurationService,
        guild_id: String,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_verified(configuration)?;
        if find_guild(&self.snapshot.guilds, &guild_id).is_none() {
            return Err(DonBotConfigurationError::UnknownGuild);
        }
        let mut settings = configuration.snapshot().settings;
        if settings.donbot.selected_guild_id != guild_id {
            clear_delivery(&mut settings.donbot);
        }
        settings.donbot.selected_guild_id = guild_id.clone();
        self.save(configuration, settings, Source::Saved)?;
        self.snapshot.selected_guild_id = guild_id;
        self.changed();
        Ok(())
    }

    pub fn set_discord_delivery_enabled(
        &mut self,
        configuration: &mut ConfigurationService,
        enabled: bool,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_verified(configuration)?;
        let mut settings = configuration.snapshot().settings;
        settings.donbot.discord_delivery_enabled = enabled;
        if enabled && !authorized_donbot_delivery(&settings, &self.snapshot).requested() {
            return Err(DonBotConfigurationError::DeliveryUnavailable);
        }
        if !settings.donbot.discord_channel_override_explicit {
            settings.donbot.selected_discord_channel_id.clear();
        }
        self.save(configuration, settings, Source::Saved)?;
        self.changed();
        Ok(())
    }

    pub fn select_discord_channel(
        &mut self,
        configuration: &mut ConfigurationService,
        channel_id: String,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_verified(configuration)?;
        let mut settings = configuration.snapshot().settings;
        let guild = find_guild(&self.snapshot.guilds, &settings.donbot.selected_guild_id)
            .ok_or(DonBotConfigurationError::DeliveryUnavailable)?;
        let policy = &guild.discord_delivery;
        if !self.snapshot.discord_summary_delivery_v1 || !policy.enabled {
            return Err(DonBotConfigurationError::DeliveryUnavailable);
        }
        if channel_id.is_empty() {
            if !policy.defaults_available {
                return Err(DonBotConfigurationError::DeliveryUnavailable);
            }
        } else if !has_channel(policy, &channel_id) {
            return Err(DonBotConfigurationError::UnknownChannel);
        }
        settings.donbot.discord_channel_override_explicit = !channel_id.is_empty();
        settings.donbot.selected_discord_channel_id = channel_id;
        self.save(configuration, settings, Source::Saved)?;
        self.changed();
        Ok(())
    }

    pub fn disconnect(
        &mut self,
        configuration: &mut ConfigurationService,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_idle(configuration)?;
        let mut settings = configuration.snapshot().settings;
        disable(&mut settings.donbot);
        self.save(configuration, settings, Source::Saved)?;
        self.clear_identity();
        self.snapshot.state = DonBotConfigurationState::Unverified;
        self.snapshot.api_base_url =
            normalize_donbot_api_base(&configuration.snapshot().settings.donbot.api_base_url)
                .ok_or(DonBotConfigurationError::InvalidConfiguration)?;
        guarded(|| configuration.erase_secret(SecretId::DonBotGw2ApiKey))
            .map_err(|()| self.fail(DonBotConfigurationError::SecretEraseFailed, Source::Saved))?;
        self.changed();
        Ok(())
    }

    pub fn shutdown(&mut self) {
        if self.snapshot.shutting_down {
            return;
        }
        self.snapshot.shutting_down = true;
        self.snapshot.state = DonBotConfigurationState::ShuttingDown;
        self.pending = None;
        self.clear_identity();
        self.snapshot.diagnostic = None;
        let _ = catch_unwind(AssertUnwindSafe(|| self.verifier.cancel_pending()));
        self.advance();
    }

    fn require_running(
        &self,
        configuration: &ConfigurationService,
    ) -> Result<(), DonBotConfigurationError> {
        if self.snapshot.shutting_down || configuration.is_shutting_down() {
            Err(DonBotConfigurationError::ShuttingDown)
        } else if self.snapshot.revision >= u64::MAX - 1 {
            Err(DonBotConfigurationError::RevisionExhausted)
        } else {
            Ok(())
        }
    }
    fn require_idle(
        &self,
        configuration: &ConfigurationService,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_running(configuration)?;
        if self.pending.is_some() {
            Err(DonBotConfigurationError::Busy)
        } else {
            Ok(())
        }
    }
    fn require_verified(
        &mut self,
        configuration: &ConfigurationService,
    ) -> Result<(), DonBotConfigurationError> {
        self.require_idle(configuration)?;
        if self.snapshot.state != DonBotConfigurationState::Verified {
            return Err(DonBotConfigurationError::NotVerified);
        }
        if normalize_donbot_api_base(&configuration.snapshot().settings.donbot.api_base_url)
            .as_deref()
            != Some(self.snapshot.api_base_url.as_str())
        {
            return Err(self.fail(DonBotConfigurationError::StaleVerification, Source::Saved));
        }
        Ok(())
    }
    fn save(
        &mut self,
        configuration: &mut ConfigurationService,
        settings: Settings,
        source: Source,
    ) -> Result<(), DonBotConfigurationError> {
        guarded(|| configuration.save_settings(settings))
            .map_err(|()| self.fail(DonBotConfigurationError::SettingsSaveFailed, source))
    }
    fn fail(
        &mut self,
        error: DonBotConfigurationError,
        source: Source,
    ) -> DonBotConfigurationError {
        if source != Source::Refresh {
            self.clear_identity();
            self.snapshot.state = DonBotConfigurationState::Error;
        }
        self.snapshot.refreshing = false;
        self.snapshot.diagnostic = Some(error);
        self.advance();
        error
    }
    fn clear_identity(&mut self) {
        self.snapshot.account_name = None;
        self.snapshot.guilds.clear();
        self.snapshot.selected_guild_id.clear();
        self.snapshot.discord_summary_delivery_v1 = false;
        self.snapshot.discord_aggregate_delivery_v1 = false;
        self.snapshot.refreshing = false;
    }
    fn changed(&mut self) {
        self.snapshot.diagnostic = None;
        self.advance();
    }
    fn advance(&mut self) {
        self.snapshot.revision += 1;
    }
}
impl Drop for DonBotConfigurationController {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn guarded<T, E>(call: impl FnOnce() -> Result<T, E>) -> Result<T, ()> {
    catch_unwind(AssertUnwindSafe(call))
        .map_err(|_| ())?
        .map_err(|_| ())
}
fn clear_delivery(settings: &mut DonBotSettings) {
    settings.discord_delivery_enabled = false;
    settings.discord_channel_override_explicit = false;
    settings.selected_discord_channel_id.clear();
}
fn disable(settings: &mut DonBotSettings) {
    settings.enabled = false;
    settings.selected_guild_id.clear();
    clear_delivery(settings);
}
fn find_guild<'a>(guilds: &'a [DonBotGuild], id: &str) -> Option<&'a DonBotGuild> {
    guilds.iter().find(|g| g.guild_id == id)
}
fn has_summary(policy: &DonBotDiscordDeliveryPolicy) -> bool {
    policy.pve_summary || policy.wvw_summary || policy.wvw_advanced || policy.wvw_stream
}
fn has_channel(policy: &DonBotDiscordDeliveryPolicy, channel: &str) -> bool {
    policy.channel_override_allowed && policy.channels.iter().any(|c| c.channel_id == channel)
}
fn sanitize(settings: &mut DonBotSettings, identity: &DonBotVerification) {
    let Some(guild) = find_guild(&identity.guilds, &settings.selected_guild_id) else {
        disable(settings);
        return;
    };
    let policy = &guild.discord_delivery;
    let authorized = identity.discord_summary_delivery_v1 && policy.enabled;
    if !settings.enabled
        || !authorized
        || (settings.discord_channel_override_explicit
            && !has_channel(policy, &settings.selected_discord_channel_id))
    {
        clear_delivery(settings);
        return;
    }
    if !settings.discord_channel_override_explicit {
        settings.selected_discord_channel_id.clear();
    }
    if !has_summary(policy)
        || (!settings.discord_channel_override_explicit && !policy.defaults_available)
    {
        settings.discord_delivery_enabled = false;
    }
}

pub fn authorized_donbot_route(
    settings: &Settings,
    snapshot: &DonBotConfigurationSnapshot,
) -> DonBotDeliveryRoute {
    if !settings.donbot.enabled
        || snapshot.shutting_down
        || snapshot.state != DonBotConfigurationState::Verified
        || snapshot.account_name.is_none()
        || !snapshot.discord_summary_delivery_v1
        || normalize_donbot_api_base(&settings.donbot.api_base_url).as_deref()
            != Some(snapshot.api_base_url.as_str())
        || settings.donbot.selected_guild_id != snapshot.selected_guild_id
    {
        return DonBotDeliveryRoute::None;
    }
    let Some(guild) = find_guild(&snapshot.guilds, &settings.donbot.selected_guild_id) else {
        return DonBotDeliveryRoute::None;
    };
    let policy = &guild.discord_delivery;
    if !policy.enabled {
        return DonBotDeliveryRoute::None;
    }
    if settings.donbot.discord_channel_override_explicit {
        if has_channel(policy, &settings.donbot.selected_discord_channel_id) {
            return DonBotDeliveryRoute::ChannelOverride {
                channel_id: settings.donbot.selected_discord_channel_id.clone(),
            };
        }
    } else if policy.defaults_available {
        return DonBotDeliveryRoute::GuildDefaults;
    }
    DonBotDeliveryRoute::None
}
pub fn authorized_donbot_delivery(
    settings: &Settings,
    snapshot: &DonBotConfigurationSnapshot,
) -> DonBotDeliveryRoute {
    if !settings.donbot.discord_delivery_enabled
        || !find_guild(&snapshot.guilds, &settings.donbot.selected_guild_id)
            .is_some_and(|g| has_summary(&g.discord_delivery))
    {
        return DonBotDeliveryRoute::None;
    }
    authorized_donbot_route(settings, snapshot)
}
pub fn authorized_donbot_aggregate_route(
    settings: &Settings,
    snapshot: &DonBotConfigurationSnapshot,
) -> DonBotDeliveryRoute {
    let route = authorized_donbot_route(settings, snapshot);
    if route == DonBotDeliveryRoute::GuildDefaults
        && !find_guild(&snapshot.guilds, &settings.donbot.selected_guild_id)
            .is_some_and(|g| g.discord_delivery.aggregate_defaults_available)
    {
        return DonBotDeliveryRoute::None;
    }
    route
}
