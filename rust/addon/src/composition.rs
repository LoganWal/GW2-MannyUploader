//! Native dependencies constructed outside Nexus callbacks.

use manny_core::{application::*, config::*, domain::*, evtc::*, filesystem::*, providers::*};
use std::{
    os::windows::ffi::OsStrExt,
    path::Path,
    sync::{Arc, Mutex},
    time::{Instant, SystemTime},
};

use crate::{
    directory_monitor::WindowsDirectoryMonitor, dpapi::DpapiSecretProtector,
    external_actions::WindowsExternalActionLauncher, file_commit::WindowsFileCommit,
    random::WindowsRandom,
};

#[cfg(test)]
mod tests;

type Secrets = Arc<Mutex<Box<dyn SecretStore>>>;

pub const PACKAGED_TWITCH_CLIENT_ID: &str = env!("MANNY_PACKAGED_TWITCH_CLIENT_ID");

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompositionError {
    Paths,
    Settings,
    History,
    Http,
    Workers,
    Workflows,
    Discovery,
    Runtime,
}

#[derive(Clone, Copy)]
pub struct NativeClock;
impl Clock for NativeClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::now()
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}

struct SharedSecrets {
    store: Secrets,
    mode: SecretProtectionMode,
}
impl SharedSecrets {
    fn lock(
        &self,
        id: SecretId,
    ) -> Result<std::sync::MutexGuard<'_, Box<dyn SecretStore>>, SecretStoreError> {
        self.store.lock().map_err(|_| SecretStoreError {
            id: Some(id),
            code: SecretStoreErrorCode::InvalidConfiguration,
            system_error: None,
        })
    }
}
impl SecretStore for SharedSecrets {
    fn mode(&self) -> SecretProtectionMode {
        self.mode
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        self.lock(id)?.load(id)
    }
    fn store(&mut self, id: SecretId, value: &SecretValue) -> Result<(), SecretStoreError> {
        self.lock(id)?.store(id, value)
    }
    fn erase(&mut self, id: SecretId) -> Result<(), SecretStoreError> {
        self.lock(id)?.erase(id)
    }
}

fn files() -> Box<dyn FileStore> {
    Box::new(FilesystemStore::new(WindowsFileCommit))
}

fn protected_store(directory: &Path) -> Result<Box<dyn SecretStore>, SecretStoreError> {
    let protector = DpapiSecretProtector::new().map_err(|error| SecretStoreError {
        id: None,
        code: SecretStoreErrorCode::Protection(error.code),
        system_error: error.system_error,
    })?;
    Ok(Box::new(ProtectedFileSecretStore::new(
        files(),
        Box::new(protector),
        directory.join("secrets"),
    )?))
}

/// The caller owns this runtime and must join its owner thread before unloading the DLL.
pub fn compose(
    game: &Path,
    addon: &Path,
    twitch_client_id: String,
) -> Result<ApplicationRuntime<NativeClock>, CompositionError> {
    #[cfg(not(feature = "lifecycle-test"))]
    let http: Arc<dyn HttpClient> = Arc::new(
        crate::http::CurlHttpClient::new(HttpTransportPolicy::HttpsOnly)
            .map_err(|_| CompositionError::Http)?,
    );
    #[cfg(feature = "lifecycle-test")]
    let http: Arc<dyn HttpClient> = Arc::new(crate::lifecycle_test::BlockingHttp::new());
    compose_with(game, addon, twitch_client_id, http, protected_store(addon))
}

fn compose_with(
    game: &Path,
    addon: &Path,
    twitch_client_id: String,
    http: Arc<dyn HttpClient>,
    secrets: Result<Box<dyn SecretStore>, SecretStoreError>,
) -> Result<ApplicationRuntime<NativeClock>, CompositionError> {
    if !game.is_absolute()
        || !addon.is_absolute()
        || game.as_os_str().encode_wide().any(|unit| unit == 0)
        || addon.as_os_str().encode_wide().any(|unit| unit == 0)
    {
        return Err(CompositionError::Paths);
    }
    let root = game.join("arcdps.cbtlogs");
    let defaults = Settings::new(root.to_str().ok_or(CompositionError::Paths)?.into());
    let settings = JsonSettingsStore::new(files(), addon.join("settings.json"), defaults)
        .map_err(|_| CompositionError::Settings)?;
    // Providers acquire only the inner store. They never enter ConfigurationService while locked.
    let (configuration_secrets, provider_secrets) = match secrets {
        Ok(store) => {
            let mode = store.mode();
            let store = Arc::new(Mutex::new(store));
            let proxy: Box<dyn SecretStore> = Box::new(SharedSecrets {
                store: store.clone(),
                mode,
            });
            (Ok(proxy), Some(store))
        }
        Err(error) => (Err(error), None),
    };
    let configuration = ConfigurationService::new(Box::new(settings), configuration_secrets)
        .map_err(|_| CompositionError::Settings)?;
    let settings = configuration.snapshot().settings;
    let history = JsonUploadHistoryStore::new(files(), addon.join("upload-history.json"), 10_000)
        .map_err(|_| CompositionError::History)?;
    let donbot_client = Arc::new(DonBotClient::new(http.clone()));
    let verifier = DonBotVerificationWorker::new(donbot_client.clone(), 1)
        .map_err(|_| CompositionError::Workers)?;
    let donbot = DonBotConfigurationController::new(&configuration, Box::new(verifier))
        .map_err(|_| CompositionError::Workflows)?;
    let configuration = Arc::new(Mutex::new(configuration));
    let twitch_client = Arc::new(TwitchClient::unconfigured(http.clone()));
    let clock = Arc::new(NativeClock);
    let sessions = Arc::new(TwitchSessionOwner::new(
        configuration.clone(),
        twitch_client.clone(),
        clock.clone(),
    ));
    let auth = TwitchAuthenticationWorker::new(twitch_client.clone(), 1)
        .map_err(|_| CompositionError::Workers)?;
    let twitch = TwitchAuthenticationController::new(
        configuration.clone(),
        Box::new(auth),
        sessions.clone(),
        clock.clone(),
    )
    .map_err(|_| CompositionError::Workflows)?;
    let test_message = TwitchTestMessageWorker::new(twitch_client.clone(), sessions.clone(), 1)
        .map_err(|_| CompositionError::Workers)?;
    let aggregate = DonBotAggregateWorker::new(donbot_client.clone(), provider_secrets.clone())
        .map_err(|_| CompositionError::Workers)?;
    let worker_options = WorkerOptions {
        queue_capacity: 8,
        parallelism: settings.general.parallel_uploads_per_provider as usize,
    };
    let random = Arc::new(WindowsRandom);
    let dps = DpsReportProviderWorker::new(
        Arc::new(DpsReportClient::new(http.clone(), random.clone())),
        provider_secrets.clone(),
        worker_options,
        DpsReportOptions {
            detailed_wvw: settings.dps_report.detailed_wvw,
        },
    )
    .map_err(|_| CompositionError::Workers)?;
    let wingman =
        WingmanProviderWorker::new(Arc::new(WingmanClient::new(http, random)), worker_options)
            .map_err(|_| CompositionError::Workers)?;
    let donbot_worker = DonBotProviderWorker::new(
        donbot_client,
        provider_secrets,
        DonBotOptions {
            api_base_url: settings.donbot.api_base_url.clone(),
            guild_id: settings.donbot.selected_guild_id.clone(),
            delivery: authorized_donbot_delivery(&settings, &donbot.snapshot()),
        },
        worker_options,
    )
    .map_err(|_| CompositionError::Workers)?;
    let chat = TwitchProviderWorker::new(
        twitch_client.clone(),
        sessions,
        TwitchProviderConfig {
            message_template: settings.twitch.message_template.clone(),
            post_success: settings.twitch.post_success,
            post_failure: settings.twitch.post_failure,
        },
        worker_options,
    )
    .map_err(|_| CompositionError::Workers)?;
    let uploads = UploadCoordinator::new(
        NativeClock,
        vec![
            Box::new(dps),
            Box::new(wingman),
            Box::new(donbot_worker),
            Box::new(chat),
        ],
        settings.general.recent_log_limit as usize,
    )
    .map_err(|_| CompositionError::Runtime)?;
    let reader = ZevtcArchiveReader::new(ArchiveLimits::default())
        .map_err(|_| CompositionError::Discovery)?;
    let parser = MetadataParserWorker::new(reader, settings.general.parser_queue_capacity as usize)
        .map_err(|_| CompositionError::Workers)?;
    let source_options = CandidateSourceOptions {
        root: settings.general.log_directory.clone().into(),
        recursive: settings.general.watch_subdirectories,
        max_candidates: settings.general.max_candidates as usize,
        minimum_last_write_time: None,
    };
    let source =
        PollingLogSource::new(source_options.clone()).map_err(|_| CompositionError::Discovery)?;
    let source = NotifyingLogSource::new(
        Box::new(source),
        Some(Box::<WindowsDirectoryMonitor>::default()),
        source_options,
    )
    .map_err(|_| CompositionError::Discovery)?;
    let pump = ApplicationPump::new(
        Box::new(source),
        LogIngestionCoordinator::new(uploads, Box::new(parser)),
        PumpOptions {
            required_matches: settings.general.stability_observations as usize,
            ..PumpOptions::default()
        },
    )
    .map_err(|_| CompositionError::Discovery)?;
    let options = NexusOptionsController::new(
        configuration.clone(),
        donbot,
        twitch,
        TwitchTestMessageController::new(Box::new(test_message)),
        Box::new(WindowsExternalActionLauncher),
        NexusOptionsControllerConfig::default(),
    )
    .map_err(|_| CompositionError::Workflows)?;
    ApplicationRuntime::new(
        RuntimeParts {
            pump,
            options,
            aggregate: DonBotAggregateDeliveryController::new(Box::new(aggregate)),
            history: Box::new(history),
            configuration,
            twitch_application: twitch_client,
            launcher: Box::new(WindowsExternalActionLauncher),
            clock,
        },
        twitch_client_id,
    )
    .map_err(|_| CompositionError::Runtime)
}
