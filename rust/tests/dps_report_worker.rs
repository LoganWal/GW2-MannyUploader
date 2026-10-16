use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{
    Arc, Mutex,
    mpsc::{self, Receiver, SyncSender},
};
use std::time::{Duration, Instant, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;

const WAIT: Duration = Duration::from_secs(5);
type Secrets = Arc<Mutex<Box<dyn SecretStore>>>;
type Events = Arc<Mutex<Vec<&'static str>>>;
struct Client<F>(F);
impl<F> DpsReportUploader for Client<F>
where
    F: Fn(
            &LogFileIdentity,
            Option<&SecretValue>,
            DpsReportOptions,
            &StopToken,
        ) -> Result<DpsReportSuccess, DpsReportError>
        + Send
        + Sync,
{
    fn upload(
        &self,
        file: &LogFileIdentity,
        token: Option<&SecretValue>,
        options: DpsReportOptions,
        stop: &StopToken,
    ) -> Result<DpsReportSuccess, DpsReportError> {
        (self.0)(file, token, options, stop)
    }
}
fn client(
    f: impl Fn(
        &LogFileIdentity,
        Option<&SecretValue>,
        DpsReportOptions,
        &StopToken,
    ) -> Result<DpsReportSuccess, DpsReportError>
    + Send
    + Sync
    + 'static,
) -> Arc<dyn DpsReportUploader> {
    Arc::new(Client(f))
}
fn success(replacement: bool) -> DpsReportSuccess {
    DpsReportSuccess {
        report: DpsReportResult {
            permalink: "https://dps.report/fixture".into(),
            boss_id: 123,
            encounter_name: "Boss".into(),
            mode: String::new(),
            success: true,
        },
        replacement_user_token: replacement
            .then(|| SecretValue::from_bytes(b"synthetic-new".to_vec())),
        warning: None,
    }
}
fn request(id: u64) -> UploadRequest {
    UploadRequest {
        job_id: UploadJobId::new(id).unwrap(),
        provider: Provider::DpsReport,
        attempt: 3,
        file: LogFileIdentity {
            canonical_path: format!("fixture-{id}.zevtc").into(),
            size: 7,
            last_write_time: SystemTime::UNIX_EPOCH,
        },
        metadata: EncounterMetadata {
            boss_id: 123,
            pov_account: "Fixture.1234".into(),
            remaining_health_basis_points: None,
        },
        dps_report_result: None,
        intent: UploadIntent::Automatic,
    }
}
fn worker(client: Arc<dyn DpsReportUploader>, secrets: Option<Secrets>) -> DpsReportProviderWorker {
    DpsReportProviderWorker::new(
        client,
        secrets,
        WorkerOptions::default(),
        DpsReportOptions::default(),
    )
    .unwrap()
}
fn result(worker: &mut DpsReportProviderWorker) -> UploadResult {
    worker
        .wait_for_result(WAIT)
        .expect("worker result should arrive")
}
fn store_error(code: SecretStoreErrorCode) -> SecretStoreError {
    SecretStoreError {
        id: Some(SecretId::DpsReportUserToken),
        code,
        system_error: None,
    }
}
struct Store {
    events: Events,
    load: u8,
    save: u8,
    saving: Option<SyncSender<()>>,
    release: Option<Receiver<()>>,
}
impl SecretStore for Store {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::NativeUserScoped
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        assert_eq!(id, SecretId::DpsReportUserToken);
        self.events.lock().unwrap().push("load");
        match self.load {
            0 => Ok(SecretValue::from_bytes(b"synthetic-current".to_vec())),
            1 => Err(store_error(SecretStoreErrorCode::NotFound)),
            2 => Err(store_error(SecretStoreErrorCode::CorruptRecord)),
            _ => panic!("private load panic"),
        }
    }
    fn store(&mut self, id: SecretId, secret: &SecretValue) -> Result<(), SecretStoreError> {
        assert_eq!(id, SecretId::DpsReportUserToken);
        assert!(secret.expose() == b"synthetic-new");
        self.events.lock().unwrap().push("store-start");
        if let Some(saving) = &self.saving {
            saving.send(()).unwrap();
        }
        if let Some(release) = &self.release {
            release.recv_timeout(WAIT).unwrap();
        }
        match self.save {
            0 => {
                self.events.lock().unwrap().push("store-finish");
                Ok(())
            }
            1 => Err(store_error(SecretStoreErrorCode::File(
                FileError::WriteFailed,
            ))),
            _ => panic!("private store panic"),
        }
    }
    fn erase(&mut self, _: SecretId) -> Result<(), SecretStoreError> {
        panic!("upload must not erase tokens")
    }
}
fn store(events: &Events, load: u8, save: u8) -> Secrets {
    Arc::new(Mutex::new(Box::new(Store {
        events: events.clone(),
        load,
        save,
        saving: None,
        release: None,
    })))
}

#[test]
fn replacement_is_persisted_before_success_is_published() {
    let events = Arc::new(Mutex::new(Vec::new()));
    let (saving, started) = mpsc::sync_channel(1);
    let (release, wait) = mpsc::sync_channel(1);
    let secrets: Secrets = Arc::new(Mutex::new(Box::new(Store {
        events: events.clone(),
        load: 0,
        save: 0,
        saving: Some(saving),
        release: Some(wait),
    })));
    let observed = events.clone();
    let mut worker = worker(
        client(move |_, token, _, _| {
            assert!(token.unwrap().expose() == b"synthetic-current");
            observed.lock().unwrap().push("upload");
            Ok(success(true))
        }),
        Some(secrets),
    );
    worker.enqueue(request(42)).unwrap();
    started.recv_timeout(WAIT).unwrap();
    assert!(worker.try_take_result().unwrap().is_none());
    assert_eq!(*events.lock().unwrap(), ["load", "upload", "store-start"]);
    release.send(()).unwrap();
    let result = result(&mut worker);
    assert_eq!(
        (result.job_id.get(), result.attempt, result.provider),
        (42, 3, Provider::DpsReport)
    );
    assert!(matches!(
        result.outcome,
        UploadOutcome::Succeeded(UploadSuccess::DpsReport(_))
    ));
    assert_eq!(
        *events.lock().unwrap(),
        ["load", "upload", "store-start", "store-finish"]
    );
}

#[test]
fn missing_and_unavailable_storage_keep_anonymous_uploads_available() {
    let events = Arc::new(Mutex::new(Vec::new()));
    for available in [false, true] {
        let mut worker = worker(
            client(|_, token, _, _| {
                assert!(token.is_none());
                Ok(success(true))
            }),
            available.then(|| store(&events, 1, 0)),
        );
        worker.enqueue(request(1)).unwrap();
        let outcome = result(&mut worker).outcome;
        if available {
            assert!(matches!(outcome, UploadOutcome::Succeeded(_)));
        } else {
            assert!(matches!(
                outcome,
                UploadOutcome::SucceededWithNotice(_, UploadNotice::CredentialStorageUnavailable)
            ));
        }
    }
}

#[test]
fn credential_failures_and_panics_never_publish_report_success() {
    for (load, save, network) in [(2, 0, false), (3, 0, false), (0, 1, true), (0, 2, true)] {
        let events = Arc::new(Mutex::new(Vec::new()));
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let mut worker = worker(
            client(move |_, _, _, _| {
                count.fetch_add(1, Ordering::Relaxed);
                Ok(success(true))
            }),
            Some(store(&events, load, save)),
        );
        worker.enqueue(request(1)).unwrap();
        let result = result(&mut worker);
        assert_eq!(result.outcome, UploadOutcome::Failed);
        assert_eq!(calls.load(Ordering::Relaxed), usize::from(network));
        assert!(!format!("{result:?}").contains("private"));
        assert!(!format!("{result:?}").contains("synthetic"));
    }
    let mut worker = worker(client(|_, _, _, _| panic!("private client panic")), None);
    worker.enqueue(request(1)).unwrap();
    assert_eq!(result(&mut worker).outcome, UploadOutcome::Failed);
}

#[test]
fn poisoned_store_fails_closed_before_upload_and_same_token_does_not_write() {
    let events = Arc::new(Mutex::new(Vec::new()));
    let secrets = store(&events, 0, 0);
    let poisoned = secrets.clone();
    let _ = std::thread::spawn(move || {
        let _lock = poisoned.lock().unwrap();
        panic!("synthetic poison");
    })
    .join();
    let mut worker = worker(
        client(|_, _, _, _| panic!("must not upload")),
        Some(secrets),
    );
    worker.enqueue(request(1)).unwrap();
    assert_eq!(result(&mut worker).outcome, UploadOutcome::Failed);
    let mut worker = DpsReportProviderWorker::new(
        client(|_, _, _, _| Ok(success(false))),
        Some(store(&events, 0, 2)),
        WorkerOptions::default(),
        DpsReportOptions::default(),
    )
    .unwrap();
    worker.enqueue(request(2)).unwrap();
    assert!(matches!(
        result(&mut worker).outcome,
        UploadOutcome::Succeeded(_)
    ));
    assert_eq!(*events.lock().unwrap(), ["load"]);
}

struct Call {
    file: LogFileIdentity,
    options: DpsReportOptions,
    release: SyncSender<()>,
}
fn controlled(capacity: usize, parallelism: usize) -> (DpsReportProviderWorker, Receiver<Call>) {
    let (started, calls) = mpsc::sync_channel(32);
    let client = client(move |file, _, options, _| {
        let (release, wait) = mpsc::sync_channel(1);
        started
            .send(Call {
                file: file.clone(),
                options,
                release,
            })
            .unwrap();
        wait.recv_timeout(WAIT).unwrap();
        Ok(success(false))
    });
    (
        DpsReportProviderWorker::new(
            client,
            None,
            WorkerOptions {
                queue_capacity: capacity,
                parallelism,
            },
            DpsReportOptions::default(),
        )
        .unwrap(),
        calls,
    )
}
#[test]
fn queued_requests_keep_captured_configuration_and_fifo_with_bounded_backpressure() {
    let (mut worker, calls) = controlled(1, 1);
    worker.enqueue(request(1)).unwrap();
    let first = calls.recv_timeout(WAIT).unwrap();
    worker
        .configure(ProviderConfiguration {
            parallelism: 1,
            policy: ProviderPolicy::DpsReport { detailed_wvw: true },
        })
        .unwrap();
    worker.enqueue(request(2)).unwrap();
    worker.update_config(DpsReportOptions::default());
    assert_eq!(worker.enqueue(request(3)), Err(ProviderError::QueueFull));
    first.release.send(()).unwrap();
    let second = calls.recv_timeout(WAIT).unwrap();
    assert!(!first.options.detailed_wvw);
    assert!(second.options.detailed_wvw);
    assert_eq!(second.file.canonical_path, request(2).file.canonical_path);
    worker.enqueue(request(3)).unwrap();
    second.release.send(()).unwrap();
    assert_eq!(result(&mut worker).job_id.get(), 1);
    assert_eq!(result(&mut worker).job_id.get(), 2);
    let third = calls.recv_timeout(WAIT).unwrap();
    assert!(!third.options.detailed_wvw);
    third.release.send(()).unwrap();
    assert_eq!(result(&mut worker).job_id.get(), 3);
}

#[test]
fn runtime_configuration_is_atomic_on_invalid_policy_parallelism_or_stop() {
    let (mut worker, _calls) = controlled(1, 1);
    let desired = ProviderConfiguration {
        parallelism: 2,
        policy: ProviderPolicy::DpsReport { detailed_wvw: true },
    };
    worker.configure(desired.clone()).unwrap();
    for invalid in [
        ProviderConfiguration {
            parallelism: 0,
            policy: ProviderPolicy::DpsReport {
                detailed_wvw: false,
            },
        },
        ProviderConfiguration {
            parallelism: 3,
            policy: ProviderPolicy::Wingman,
        },
    ] {
        assert_eq!(
            worker.configure(invalid),
            Err(ProviderConfigurationError::InvalidConfiguration)
        );
        assert_eq!(worker.snapshot().parallelism, 2);
        assert!(worker.config_snapshot().detailed_wvw);
    }
    worker.cancel_pending();
    assert_eq!(
        worker.configure(desired),
        Err(ProviderConfigurationError::Stopping)
    );
}

#[test]
fn live_parallelism_preserves_provider_admission_limits() {
    let (mut worker, calls) = controlled(3, 1);
    worker.enqueue(request(1)).unwrap();
    let first = calls.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(2)).unwrap();
    worker.enqueue(request(3)).unwrap();
    worker.update_parallelism(2).unwrap();
    let second = calls.recv_timeout(WAIT).unwrap();
    worker.update_parallelism(1).unwrap();
    first.release.send(()).unwrap();
    result(&mut worker);
    assert_eq!(worker.snapshot().active, 1);
    assert_eq!(worker.snapshot().pending, 1);
    second.release.send(()).unwrap();
    result(&mut worker);
    let third = calls.recv_timeout(WAIT).unwrap();
    third.release.send(()).unwrap();
    result(&mut worker);
    assert_eq!(
        worker.update_parallelism(0),
        Err(WorkerError::InvalidParallelism)
    );
}

#[test]
fn retries_use_positive_bounded_delays_and_warnings_remain_typed() {
    for (delay, expected) in [
        (None, 30),
        (Some(0), 30),
        (Some(1), 1),
        (Some(86400), 86400),
        (Some(86401), 30),
    ] {
        let mut worker = worker(
            client(move |_, _, _, _| {
                Err(DpsReportError {
                    disposition: DpsReportDisposition::Retry,
                    retry_after: delay.map(Duration::from_secs),
                    ..DpsReportError::failed(DpsReportErrorCode::Transport)
                })
            }),
            None,
        );
        worker.enqueue(request(1)).unwrap();
        assert_eq!(
            result(&mut worker).outcome,
            UploadOutcome::RetryAfter(Duration::from_secs(expected))
        );
    }
    for disposition in [
        DpsReportDisposition::Failed,
        DpsReportDisposition::Cancelled,
    ] {
        let mut worker = worker(
            client(move |_, _, _, _| {
                Err(DpsReportError {
                    disposition,
                    ..DpsReportError::failed(DpsReportErrorCode::Transport)
                })
            }),
            None,
        );
        worker.enqueue(request(1)).unwrap();
        assert_eq!(
            result(&mut worker).outcome,
            if disposition == DpsReportDisposition::Failed {
                UploadOutcome::Failed
            } else {
                UploadOutcome::Cancelled
            }
        );
    }
    for (warning, expected) in [
        (
            DpsReportWarning::GeneratedWithWarning,
            UploadNotice::ProviderWarning,
        ),
        (
            DpsReportWarning::InvalidReturnedToken,
            UploadNotice::InvalidReturnedCredential,
        ),
    ] {
        let mut worker = worker(
            client(move |_, _, _, _| {
                let mut result = success(false);
                result.warning = Some(warning);
                Ok(result)
            }),
            None,
        );
        worker.enqueue(request(1)).unwrap();
        assert!(
            matches!(result(&mut worker).outcome,UploadOutcome::SucceededWithNotice(_,actual) if actual == expected)
        );
    }
}

#[test]
fn cancellation_discards_queued_work_and_joins_inflight_client_before_return() {
    let (started, waiting) = mpsc::sync_channel(1);
    let stopped = Arc::new(AtomicBool::new(false));
    let observed = stopped.clone();
    let mut worker = worker(
        client(move |_, _, _, stop| {
            started.send(()).unwrap();
            let deadline = Instant::now() + WAIT;
            while !stop.is_stop_requested() {
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            observed.store(true, Ordering::Release);
            Ok(success(true))
        }),
        None,
    );
    worker.enqueue(request(1)).unwrap();
    waiting.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(2)).unwrap();
    worker.cancel_pending();
    worker.cancel_pending();
    assert_eq!(worker.enqueue(request(3)), Err(ProviderError::Stopping));
    assert!(worker.try_take_result().unwrap().is_none());
    drop(worker);
    assert!(stopped.load(Ordering::Acquire));
    assert!(waiting.try_recv().is_err());
}

#[test]
fn construction_and_enqueue_reject_invalid_requests_without_starting_uploads() {
    for options in [
        WorkerOptions {
            queue_capacity: 0,
            parallelism: 1,
        },
        WorkerOptions {
            queue_capacity: 1,
            parallelism: 0,
        },
        WorkerOptions {
            queue_capacity: 1,
            parallelism: 33,
        },
    ] {
        assert!(
            DpsReportProviderWorker::new(
                client(|_, _, _, _| panic!("must not upload")),
                None,
                options,
                DpsReportOptions::default()
            )
            .is_err()
        );
    }
    let mut worker = worker(client(|_, _, _, _| panic!("must not upload")), None);
    for mode in 0..4 {
        let mut request = request(1);
        match mode {
            0 => request.provider = Provider::Wingman,
            1 => request.attempt = 0,
            2 => request.file.canonical_path.clear(),
            _ => request.dps_report_result = Some(success(false).report),
        }
        assert_eq!(worker.enqueue(request), Err(ProviderError::InvalidRequest));
    }
    assert_eq!(worker.provider(), Provider::DpsReport);
}
