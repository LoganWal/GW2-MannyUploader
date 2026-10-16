use super::*;
use std::{
    path::PathBuf,
    sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    time::Duration,
};

static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(1);

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "manny-composition-{}-{}-\u{65e5}",
            std::process::id(),
            NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
        ));
        // create_dir rejects stale fixtures instead of reusing someone else's state.
        std::fs::create_dir(&root).unwrap();
        Self(root)
    }
    fn game(&self) -> PathBuf {
        self.0.join("game")
    }
    fn addon(&self) -> PathBuf {
        self.0.join("addon")
    }
    fn runtime(&self, http: Arc<dyn HttpClient>) -> ApplicationRuntime<NativeClock> {
        compose_with(
            &self.game(),
            &self.addon(),
            PACKAGED_TWITCH_CLIENT_ID.into(),
            http,
            protected_store(&self.addon()),
        )
        .unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

#[derive(Default)]
struct NoHttp(AtomicUsize);
impl HttpClient for NoHttp {
    fn execute(&self, _: HttpRequest, _: &StopToken) -> Result<HttpResponse, HttpError> {
        self.0.fetch_add(1, Ordering::SeqCst);
        Err(HttpError::new(HttpErrorCode::InvalidRequest))
    }
}

fn until(mut condition: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while !condition() {
        assert!(
            Instant::now() < deadline,
            "synthetic operation did not complete"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn native_defaults_use_owned_unicode_paths_and_start_without_network() {
    let fixture = Fixture::new();
    let http = Arc::new(NoHttp::default());
    let mut runtime = fixture.runtime(http.clone());
    let handle = runtime.handle();
    let snapshot = handle.snapshot();
    assert_eq!(
        PathBuf::from(
            snapshot
                .options
                .configuration
                .settings
                .general
                .log_directory
        ),
        fixture.game().join("arcdps.cbtlogs")
    );
    assert_eq!(
        snapshot
            .options
            .configuration
            .persistent_secret_storage
            .state,
        PersistentSecretStorageState::Available
    );
    let stop = CancellationSource::default();
    runtime.tick(&stop.token()).unwrap();
    assert!(!handle.snapshot().root_available);
    assert!(handle.snapshot().jobs.is_empty());
    runtime.shutdown();
    assert!(handle.snapshot().shutting_down);
    drop(runtime);
    assert_eq!(http.0.load(Ordering::SeqCst), 0);
    assert!(!fixture.addon().join("secrets").exists());
}

#[test]
fn commands_persist_only_on_owner_ticks_and_survive_restart() {
    let fixture = Fixture::new();
    let http = Arc::new(NoHttp::default());
    let mut runtime = fixture.runtime(http.clone());
    runtime
        .handle()
        .submit_options(NexusOptionsCommand::SetWingmanEnabled(false))
        .unwrap();
    assert!(!fixture.addon().join("settings.json").exists());
    runtime
        .tick(&CancellationSource::default().token())
        .unwrap();
    drop(runtime);
    let restarted = fixture.runtime(http.clone());
    assert!(
        !restarted
            .handle()
            .snapshot()
            .options
            .configuration
            .settings
            .wingman
            .enabled
    );
    assert_eq!(http.0.load(Ordering::SeqCst), 0);
}

#[test]
fn missing_protected_storage_is_visible_without_fallback() {
    let fixture = Fixture::new();
    let http = Arc::new(NoHttp::default());
    let runtime = compose_with(
        &fixture.game(),
        &fixture.addon(),
        PACKAGED_TWITCH_CLIENT_ID.into(),
        http.clone(),
        Err(SecretStoreError {
            id: None,
            code: SecretStoreErrorCode::Protection(
                SecretProtectionErrorCode::UnsupportedEnvironment,
            ),
            system_error: Some(50),
        }),
    )
    .unwrap();
    let capability = runtime
        .handle()
        .snapshot()
        .options
        .configuration
        .persistent_secret_storage;
    assert_eq!(
        capability.state,
        PersistentSecretStorageState::UnsupportedEnvironment
    );
    assert_eq!(capability.mode, None);
    assert_eq!(capability.system_error, Some(50));
    drop(runtime);
    assert!(!fixture.addon().join("secrets").exists());
    assert_eq!(http.0.load(Ordering::SeqCst), 0);
}

#[test]
fn protected_proxy_and_workers_share_rotations_and_erasure() {
    let fixture = Fixture::new();
    let store = protected_store(&fixture.addon()).unwrap();
    let mode = store.mode();
    let shared = Arc::new(Mutex::new(store));
    let mut proxy = SharedSecrets {
        store: shared.clone(),
        mode,
    };
    let id = SecretId::DpsReportUserToken;
    proxy
        .store(id, &SecretValue::from_bytes(b"synthetic-first".to_vec()))
        .unwrap();
    assert_eq!(
        shared.lock().unwrap().load(id).unwrap().expose(),
        b"synthetic-first"
    );
    shared
        .lock()
        .unwrap()
        .store(id, &SecretValue::from_bytes(b"synthetic-rotation".to_vec()))
        .unwrap();
    assert_eq!(proxy.load(id).unwrap().expose(), b"synthetic-rotation");
    assert_eq!(proxy.mode(), mode);
    let bytes = std::fs::read(fixture.addon().join("secrets/dps-report-token.bin")).unwrap();
    assert!(
        !bytes
            .windows(b"synthetic".len())
            .any(|part| part == b"synthetic")
    );
    proxy.erase(id).unwrap();
    assert_eq!(
        shared.lock().unwrap().load(id).unwrap_err().code,
        SecretStoreErrorCode::NotFound
    );
}

#[test]
fn poisoned_shared_store_fails_closed() {
    let fixture = Fixture::new();
    let store = protected_store(&fixture.addon()).unwrap();
    let mode = store.mode();
    let shared = Arc::new(Mutex::new(store));
    let mut proxy = SharedSecrets {
        store: shared.clone(),
        mode,
    };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _guard = shared.lock().unwrap();
        panic!("synthetic storage panic");
    }));
    let id = SecretId::DonBotGw2ApiKey;
    assert_eq!(
        proxy.load(id).unwrap_err().code,
        SecretStoreErrorCode::InvalidConfiguration
    );
    assert!(
        proxy
            .store(id, &SecretValue::from_bytes(b"synthetic".to_vec()))
            .is_err()
    );
    assert!(proxy.erase(id).is_err());
    assert_eq!(proxy.mode(), mode);
    assert!(!fixture.addon().join("secrets").exists());
}

#[test]
fn malformed_archive_is_parsed_retained_and_not_replayed_after_restart() {
    let fixture = Fixture::new();
    let http = Arc::new(NoHttp::default());
    let mut runtime = fixture.runtime(http.clone());
    let handle = runtime.handle();
    let root = fixture.game().join("arcdps.cbtlogs");
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(root.join("synthetic.zevtc"), b"invalid archive").unwrap();
    handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    let stop = CancellationSource::default();
    until(|| {
        runtime.tick(&stop.token()).unwrap();
        handle
            .snapshot()
            .jobs
            .first()
            .is_some_and(|job| job.record.providers[0].state == ProviderState::Failed)
    });
    assert!(fixture.addon().join("upload-history.json").exists());
    assert_eq!(handle.snapshot().jobs.len(), 1);
    drop(runtime);
    let mut runtime = fixture.runtime(http.clone());
    let handle = runtime.handle();
    handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    runtime.tick(&stop.token()).unwrap();
    assert_eq!(handle.snapshot().jobs.len(), 1);
    assert_eq!(http.0.load(Ordering::SeqCst), 0);
}

struct BlockingHttp {
    entered: AtomicBool,
    exited: AtomicBool,
}
impl HttpClient for BlockingHttp {
    fn execute(&self, _: HttpRequest, stop: &StopToken) -> Result<HttpResponse, HttpError> {
        self.entered.store(true, Ordering::SeqCst);
        stop.wait_for_stop(Duration::from_secs(10));
        self.exited.store(true, Ordering::SeqCst);
        Err(HttpError::new(HttpErrorCode::Cancelled))
    }
}

#[test]
fn native_owner_shutdown_joins_active_authentication_http() {
    let fixture = Fixture::new();
    let http = Arc::new(BlockingHttp {
        entered: AtomicBool::new(false),
        exited: AtomicBool::new(false),
    });
    let runtime = fixture.runtime(http.clone());
    let mut owner = ApplicationRuntimeThread::start(runtime).unwrap();
    let handle = owner.handle();
    handle
        .submit_options(NexusOptionsCommand::ConnectTwitch)
        .unwrap();
    until(|| http.entered.load(Ordering::SeqCst));
    owner.shutdown();
    assert!(http.exited.load(Ordering::SeqCst));
    assert!(handle.snapshot().shutting_down);
    assert!(
        handle
            .submit_options(NexusOptionsCommand::ConnectTwitch)
            .is_err()
    );
    owner.shutdown();
}

#[test]
fn invalid_paths_and_client_identity_fail_without_external_work() {
    let fixture = Fixture::new();
    let http = Arc::new(NoHttp::default());
    assert!(matches!(
        compose_with(
            Path::new("relative"),
            &fixture.addon(),
            String::new(),
            http.clone(),
            protected_store(&fixture.addon())
        ),
        Err(CompositionError::Paths)
    ));
    assert!(matches!(
        compose_with(
            &fixture.game(),
            &fixture.addon(),
            "invalid client id".into(),
            http.clone(),
            protected_store(&fixture.addon())
        ),
        Err(CompositionError::Runtime)
    ));
    assert_eq!(http.0.load(Ordering::SeqCst), 0);
    assert!(!fixture.addon().exists());
}

fn synthetic_archive() -> Vec<u8> {
    let mut payload = b"EVTC20261016\x01\x7b\0\0".to_vec();
    payload.extend_from_slice(&1u32.to_le_bytes());
    let mut agent = [0u8; 96];
    agent[..8].copy_from_slice(&42u64.to_le_bytes());
    let name = b"Fixture\0:Synthetic.1234\0";
    agent[28..28 + name.len()].copy_from_slice(name);
    payload.extend_from_slice(&agent);
    payload.extend_from_slice(&0u32.to_le_bytes());
    let mut event = [0u8; 64];
    event[8..16].copy_from_slice(&42u64.to_le_bytes());
    event[56] = 13;
    payload.extend_from_slice(&event);
    let name = b"fixture.evtc";
    let crc = crc32fast::hash(&payload);
    let mut local = vec![0u8; 30];
    local[..4].copy_from_slice(b"PK\x03\x04");
    local[4..6].copy_from_slice(&20u16.to_le_bytes());
    local[14..18].copy_from_slice(&crc.to_le_bytes());
    local[18..22].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    local[22..26].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    local[26..28].copy_from_slice(&(name.len() as u16).to_le_bytes());
    local.extend_from_slice(name);
    local.extend_from_slice(&payload);
    let directory_offset = local.len() as u32;
    let mut central = vec![0u8; 46];
    central[..4].copy_from_slice(b"PK\x01\x02");
    central[4..6].copy_from_slice(&20u16.to_le_bytes());
    central[6..8].copy_from_slice(&20u16.to_le_bytes());
    central[16..20].copy_from_slice(&crc.to_le_bytes());
    central[20..24].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    central[24..28].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    central[28..30].copy_from_slice(&(name.len() as u16).to_le_bytes());
    central.extend_from_slice(name);
    let mut end = [0u8; 22];
    end[..4].copy_from_slice(b"PK\x05\x06");
    end[8..10].copy_from_slice(&1u16.to_le_bytes());
    end[10..12].copy_from_slice(&1u16.to_le_bytes());
    end[12..16].copy_from_slice(&(central.len() as u32).to_le_bytes());
    end[16..20].copy_from_slice(&directory_offset.to_le_bytes());
    local.extend_from_slice(&central);
    local.extend_from_slice(&end);
    local
}

struct DpsHttp {
    history: PathBuf,
    calls: AtomicUsize,
}
impl HttpClient for DpsHttp {
    fn execute(
        &self,
        mut request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, HttpError> {
        self.calls.fetch_add(1, Ordering::SeqCst);
        assert_eq!(request.url, DPS_REPORT_UPLOAD_URL);
        let history = JsonUploadHistoryStore::new(files(), self.history.clone(), 10_000).unwrap();
        assert_eq!(
            history.records().len(),
            1,
            "identity must be durable before HTTP starts"
        );
        let body = request.body.as_mut().unwrap();
        let expected = body.content_length() as usize;
        let mut bytes = Vec::new();
        let mut chunk = [0u8; 1024];
        loop {
            let count = body.read(&mut chunk, stop).unwrap();
            if count == 0 {
                break;
            }
            bytes.extend_from_slice(&chunk[..count]);
        }
        assert_eq!(bytes.len(), expected);
        assert!(bytes.windows(4).any(|part| part == b"PK\x03\x04"));
        Ok(HttpResponse { status_code: 200, headers: vec![], body: SecretValue::from_bytes(
            br#"{"permalink":"https://dps.report/fixture","encounter":{"bossId":123,"boss":"Fixture","success":true},"userToken":"synthetic-rotation"}"#.to_vec()) })
    }
}

#[test]
fn concrete_pipeline_uploads_fixture_persists_receipt_and_rotates_protected_token() {
    let fixture = Fixture::new();
    let http = Arc::new(DpsHttp {
        history: fixture.addon().join("upload-history.json"),
        calls: AtomicUsize::new(0),
    });
    let mut runtime = fixture.runtime(http.clone());
    let handle = runtime.handle();
    let stop = CancellationSource::default();
    handle
        .submit_options(NexusOptionsCommand::SetWingmanEnabled(false))
        .unwrap();
    runtime.tick(&stop.token()).unwrap();
    let root = fixture.game().join("arcdps.cbtlogs");
    std::fs::create_dir_all(&root).unwrap();
    std::fs::write(root.join("fixture.zevtc"), synthetic_archive()).unwrap();
    handle.select_logs(LogSelectionMode::Last24Hours).unwrap();
    until(|| {
        runtime.tick(&stop.token()).unwrap();
        handle
            .snapshot()
            .jobs
            .first()
            .is_some_and(|job| job.record.providers[0].state == ProviderState::Succeeded)
    });
    drop(runtime);
    assert_eq!(http.calls.load(Ordering::SeqCst), 1);
    assert_eq!(
        protected_store(&fixture.addon())
            .unwrap()
            .load(SecretId::DpsReportUserToken)
            .unwrap()
            .expose(),
        b"synthetic-rotation"
    );
    let history = JsonUploadHistoryStore::new(files(), http.history.clone(), 10_000).unwrap();
    assert_eq!(
        history.records()[0]
            .dps_report_result
            .as_ref()
            .unwrap()
            .permalink,
        "https://dps.report/fixture"
    );
    let mut runtime = fixture.runtime(http.clone());
    runtime
        .handle()
        .select_logs(LogSelectionMode::Last24Hours)
        .unwrap();
    runtime.tick(&stop.token()).unwrap();
    assert_eq!(runtime.handle().snapshot().jobs.len(), 1);
    assert_eq!(http.calls.load(Ordering::SeqCst), 1);
}

#[test]
fn production_transport_can_be_composed_without_sending_requests() {
    let fixture = Fixture::new();
    let runtime = compose(
        &fixture.game(),
        &fixture.addon(),
        PACKAGED_TWITCH_CLIENT_ID.into(),
    )
    .unwrap();
    let handle = runtime.handle();
    drop(runtime);
    assert!(handle.snapshot().shutting_down);
    assert!(!fixture.addon().exists());
}
