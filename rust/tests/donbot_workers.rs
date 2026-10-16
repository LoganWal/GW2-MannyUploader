use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant, SystemTime};

const WAIT: Duration = Duration::from_secs(5);

#[test]
fn runtime_donbot_configuration_validates_before_publishing_policy_or_parallelism() {
    let mut worker = worker(client(|_, _, _, _| Ok(success(false))), None, options());
    let before = worker.config_snapshot();
    for invalid in [
        ProviderConfiguration {
            parallelism: 2,
            policy: ProviderPolicy::Wingman,
        },
        ProviderConfiguration {
            parallelism: 2,
            policy: ProviderPolicy::DonBot {
                api_base_url: "http://invalid".into(),
                guild_id: "123".into(),
                delivery: DonBotDeliveryRoute::None,
            },
        },
        ProviderConfiguration {
            parallelism: 0,
            policy: ProviderPolicy::DonBot {
                api_base_url: DEFAULT_DONBOT_API.into(),
                guild_id: "456".into(),
                delivery: DonBotDeliveryRoute::None,
            },
        },
    ] {
        assert_eq!(
            worker.configure(invalid),
            Err(ProviderConfigurationError::InvalidConfiguration)
        );
        assert_eq!(worker.config_snapshot(), before);
        assert_eq!(worker.snapshot().parallelism, 1);
    }
    let desired = ProviderConfiguration {
        parallelism: 2,
        policy: ProviderPolicy::DonBot {
            api_base_url: format!("{DEFAULT_DONBOT_API}///"),
            guild_id: "456".into(),
            delivery: DonBotDeliveryRoute::GuildDefaults,
        },
    };
    worker.configure(desired.clone()).unwrap();
    assert_eq!(worker.config_snapshot().api_base_url, DEFAULT_DONBOT_API);
    assert_eq!(worker.config_snapshot().guild_id, "456");
    assert_eq!(worker.snapshot().parallelism, 2);
    worker.cancel_pending();
    assert_eq!(
        worker.configure(desired),
        Err(ProviderConfigurationError::Stopping)
    );
}
type Secrets = Arc<Mutex<Box<dyn SecretStore>>>;
enum Call<'a> {
    File(&'a LogFileIdentity),
    Link(&'a str),
    Aggregate(&'a [u64]),
}
struct Reply {
    upload: DonBotUploadSuccess,
    count: usize,
}
struct Client<F>(F);
impl<F> DonBotUploader for Client<F>
where
    F: Fn(Call<'_>, &DonBotOptions, &SecretValue, &StopToken) -> Result<Reply, DonBotError>
        + Send
        + Sync,
{
    fn upload(
        &self,
        file: &LogFileIdentity,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError> {
        (self.0)(Call::File(file), options, key, stop).map(|r| r.upload)
    }
    fn import_permalink(
        &self,
        url: &str,
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotUploadSuccess, DonBotError> {
        (self.0)(Call::Link(url), options, key, stop).map(|r| r.upload)
    }
    fn deliver_aggregate(
        &self,
        ids: &[u64],
        options: &DonBotOptions,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotAggregateSuccess, DonBotError> {
        (self.0)(Call::Aggregate(ids), options, key, stop).map(|r| DonBotAggregateSuccess {
            fight_log_count: r.count,
            discord_delivery: r.upload.discord_delivery,
        })
    }
}
fn client(
    f: impl Fn(Call<'_>, &DonBotOptions, &SecretValue, &StopToken) -> Result<Reply, DonBotError>
    + Send
    + Sync
    + 'static,
) -> Arc<dyn DonBotUploader> {
    Arc::new(Client(f))
}
fn options() -> DonBotOptions {
    DonBotOptions {
        api_base_url: "https://donbot.example/service///".into(),
        guild_id: "123".into(),
        delivery: DonBotDeliveryRoute::None,
    }
}
fn request(id: u64, imported: bool) -> UploadRequest {
    UploadRequest {
        job_id: UploadJobId::new(id).unwrap(),
        provider: Provider::DonBot,
        attempt: 3,
        file: LogFileIdentity {
            canonical_path: "absent-fixture.zevtc".into(),
            size: 9,
            last_write_time: SystemTime::UNIX_EPOCH,
        },
        metadata: EncounterMetadata {
            boss_id: 1,
            pov_account: "Fixture.1234".into(),
            remaining_health_basis_points: None,
        },
        dps_report_result: imported.then(|| DpsReportResult {
            permalink: "https://wvw.report/fixture".into(),
            encounter_name: "WvW".into(),
            boss_id: 1,
            mode: String::new(),
            success: true,
        }),
        intent: UploadIntent::Automatic,
    }
}
fn aggregate_request(id: u64) -> DonBotAggregateRequest {
    DonBotAggregateRequest {
        request_id: id,
        api_base_url: options().api_base_url,
        guild_id: "123".into(),
        fight_log_ids: vec![11, 22],
        delivery: DonBotDeliveryRoute::GuildDefaults,
    }
}
fn success(requested: bool) -> Reply {
    Reply {
        upload: DonBotUploadSuccess {
            upload_id: Some(42),
            fight_log_id: Some(99),
            discord_delivery: if requested {
                DonBotDiscordDeliveryReceipt {
                    outcome: DonBotDiscordDeliveryOutcome::Sent,
                    sent: 1,
                    ..DonBotDiscordDeliveryReceipt::default()
                }
            } else {
                DonBotDiscordDeliveryReceipt::default()
            },
        },
        count: 2,
    }
}
struct Store {
    mode: u8,
    loads: Arc<AtomicUsize>,
}
impl SecretStore for Store {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::NativeUserScoped
    }
    fn load(&self, id: SecretId) -> Result<SecretValue, SecretStoreError> {
        assert_eq!(id, SecretId::DonBotGw2ApiKey);
        let n = self.loads.fetch_add(1, Ordering::Relaxed) + 1;
        match self.mode {
            0 => Ok(SecretValue::from_bytes(
                format!("SYNTHETIC-{n}").into_bytes(),
            )),
            1 | 2 => Err(SecretStoreError {
                id: Some(id),
                code: if self.mode == 1 {
                    SecretStoreErrorCode::NotFound
                } else {
                    SecretStoreErrorCode::CorruptRecord
                },
                system_error: None,
            }),
            _ => panic!("synthetic private storage panic"),
        }
    }
    fn store(&mut self, _: SecretId, _: &SecretValue) -> Result<(), SecretStoreError> {
        panic!("worker must not store")
    }
    fn erase(&mut self, _: SecretId) -> Result<(), SecretStoreError> {
        panic!("worker must not erase")
    }
}
fn secrets(mode: u8) -> (Secrets, Arc<AtomicUsize>) {
    let loads = Arc::new(AtomicUsize::new(0));
    (
        Arc::new(Mutex::new(Box::new(Store {
            mode,
            loads: loads.clone(),
        }))),
        loads,
    )
}
fn worker(
    client: Arc<dyn DonBotUploader>,
    secrets: Option<Secrets>,
    options: DonBotOptions,
) -> DonBotProviderWorker {
    DonBotProviderWorker::new(
        client,
        secrets,
        options,
        WorkerOptions {
            queue_capacity: 1,
            parallelism: 1,
        },
    )
    .unwrap()
}
fn outcome(worker: &mut DonBotProviderWorker, id: u64) -> UploadOutcome {
    let result = worker.wait_for_result(WAIT).unwrap();
    assert_eq!(
        (result.job_id.get(), result.provider, result.attempt),
        (id, Provider::DonBot, 3)
    );
    result.outcome
}

#[test]
fn upload_routing_loads_the_key_per_attempt_and_preserves_receipt_provenance() {
    let (secrets, loads) = secrets(0);
    let store = secrets.clone();
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let mut worker = worker(
        client(move |call, options, key, _| {
            assert!(store.try_lock().is_ok());
            let n = count.fetch_add(1, Ordering::Relaxed) + 1;
            assert!(key.expose() == format!("SYNTHETIC-{n}").as_bytes());
            assert_eq!(options.api_base_url, "https://donbot.example/service");
            match call {
                Call::File(file) => assert_eq!(file, &request(1, false).file),
                Call::Link(url) => assert_eq!(url, "https://wvw.report/fixture"),
                _ => panic!("no aggregate"),
            };
            Ok(success(false))
        }),
        Some(secrets),
        options(),
    );
    for (id, imported) in [(1, false), (2, true)] {
        worker.enqueue(request(id, imported)).unwrap();
        let UploadOutcome::Succeeded(UploadSuccess::DonBot(Some(receipt))) =
            outcome(&mut worker, id)
        else {
            panic!("expected receipt")
        };
        assert_eq!(receipt.upload_id, Some(42));
        assert_eq!(receipt.fight_log_id, Some(99));
        assert_eq!(receipt.guild_id.as_deref(), Some("123"));
    }
    assert_eq!(loads.load(Ordering::Relaxed), 2);
    assert_eq!(worker.provider(), Provider::DonBot);
}

#[test]
fn queued_uploads_capture_configuration_and_reupload_suppresses_discord() {
    let (secrets, _) = secrets(0);
    let (started, ready) = mpsc::sync_channel(3);
    let mut worker = worker(
        client(move |_, options, _, _| {
            let (release, wait) = mpsc::sync_channel(1);
            started.send((options.clone(), release)).unwrap();
            wait.recv_timeout(WAIT).unwrap();
            Ok(success(options.delivery.requested()))
        }),
        Some(secrets),
        options(),
    );
    worker.enqueue(request(1, false)).unwrap();
    let first = ready.recv_timeout(WAIT).unwrap();
    let mut second = options();
    second.guild_id = "456".into();
    second.delivery = DonBotDeliveryRoute::ChannelOverride {
        channel_id: "789".into(),
    };
    worker
        .configure(ProviderConfiguration {
            parallelism: 1,
            policy: ProviderPolicy::DonBot {
                api_base_url: second.api_base_url.clone(),
                guild_id: second.guild_id.clone(),
                delivery: second.delivery.clone(),
            },
        })
        .unwrap();
    worker.enqueue(request(2, true)).unwrap();
    let mut third = second;
    third.guild_id = "321".into();
    worker.update_config(third).unwrap();
    assert_eq!(
        worker.enqueue(request(3, false)),
        Err(ProviderError::QueueFull)
    );
    first.1.send(()).unwrap();
    let second = ready.recv_timeout(WAIT).unwrap();
    assert_eq!(first.0.guild_id, "123");
    assert_eq!(second.0.guild_id, "456");
    assert!(matches!(
        second.0.delivery,
        DonBotDeliveryRoute::ChannelOverride { .. }
    ));
    let mut reupload = request(3, true);
    reupload.intent = UploadIntent::Reupload;
    worker.enqueue(reupload).unwrap();
    second.1.send(()).unwrap();
    outcome(&mut worker, 1);
    outcome(&mut worker, 2);
    let third = ready.recv_timeout(WAIT).unwrap();
    assert_eq!(third.0.guild_id, "321");
    assert_eq!(third.0.delivery, DonBotDeliveryRoute::None);
    third.1.send(()).unwrap();
    outcome(&mut worker, 3);
    assert!(matches!(
        worker.config_snapshot().delivery,
        DonBotDeliveryRoute::ChannelOverride { .. }
    ));
}

#[test]
fn upload_errors_and_notices_keep_credentials_private_and_retries_bounded() {
    for mode in 0..4 {
        let (secrets, _) = secrets(mode);
        let mut worker = worker(
            client(|_, _, _, _| panic!("must not call")),
            Some(secrets),
            options(),
        );
        if mode == 0 {
            continue;
        }
        worker.enqueue(request(1, false)).unwrap();
        assert_eq!(
            outcome(&mut worker, 1),
            match mode {
                1 => UploadOutcome::FailedWithNotice(UploadFailure::DonBotKeyMissing),
                2 => UploadOutcome::FailedWithNotice(UploadFailure::CredentialLoadFailed),
                _ => UploadOutcome::Failed,
            }
        );
    }
    let mut absent = worker(
        client(|_, _, _, _| panic!("must not call")),
        None,
        options(),
    );
    absent.enqueue(request(1, false)).unwrap();
    assert_eq!(
        outcome(&mut absent, 1),
        UploadOutcome::FailedWithNotice(UploadFailure::DonBotKeyMissing)
    );
    for (delay, expected) in [
        (None, 30),
        (Some(0), 30),
        (Some(1), 1),
        (Some(86400), 86400),
        (Some(86401), 30),
    ] {
        let (secrets, _) = secrets(0);
        let mut worker = worker(
            client(move |_, _, _, _| {
                Err(DonBotError {
                    disposition: DonBotDisposition::Retry,
                    retry_after: delay.map(Duration::from_secs),
                    ..DonBotError::failed(DonBotErrorCode::Transport)
                })
            }),
            Some(secrets),
            options(),
        );
        worker.enqueue(request(1, false)).unwrap();
        assert_eq!(
            outcome(&mut worker, 1),
            UploadOutcome::RetryAfter(Duration::from_secs(expected))
        );
    }
    for (code, expected) in [
        (
            DonBotErrorCode::UnconfirmedUpload,
            UploadOutcome::FailedWithNotice(UploadFailure::UnconfirmedUpload),
        ),
        (
            DonBotErrorCode::UnconfirmedDelivery,
            UploadOutcome::FailedWithNotice(UploadFailure::UnconfirmedDelivery),
        ),
        (
            DonBotErrorCode::InvalidKey,
            UploadOutcome::FailedWithNotice(UploadFailure::DonBotKeyMissing),
        ),
        (DonBotErrorCode::Internal, UploadOutcome::Failed),
    ] {
        let (secrets, _) = secrets(0);
        let mut worker = worker(
            client(move |_, _, _, _| Err(DonBotError::failed(code))),
            Some(secrets),
            options(),
        );
        worker.enqueue(request(1, true)).unwrap();
        assert_eq!(outcome(&mut worker, 1), expected);
    }
}

#[test]
fn upload_success_notices_preserve_ingestion_even_when_discord_fails() {
    for (delivery, notice) in [
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Sent,
                sent: 1,
                ..Default::default()
            },
            UploadNotice::DiscordSent,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: 1,
                failed: 1,
                ..Default::default()
            },
            UploadNotice::DiscordPartial,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Skipped,
                skipped: 1,
                ..Default::default()
            },
            UploadNotice::DiscordSkipped,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Failed,
                failed: 1,
                ..Default::default()
            },
            UploadNotice::DiscordFailed,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Ambiguous,
                ambiguous: 1,
                ..Default::default()
            },
            UploadNotice::DiscordAmbiguous,
        ),
    ] {
        let (secrets, _) = secrets(0);
        let mut options = options();
        options.delivery = DonBotDeliveryRoute::GuildDefaults;
        let expected = delivery.clone();
        let mut worker = worker(
            client(move |_, _, _, _| {
                let mut result = success(true);
                result.upload.discord_delivery = delivery.clone();
                Ok(result)
            }),
            Some(secrets),
            options,
        );
        worker.enqueue(request(1, true)).unwrap();
        let UploadOutcome::SucceededWithNotice(UploadSuccess::DonBot(Some(receipt)), actual) =
            outcome(&mut worker, 1)
        else {
            panic!("expected notice")
        };
        assert_eq!(actual, notice);
        assert_eq!(receipt.discord_delivery, expected);
        assert_eq!(receipt.fight_log_id, Some(99));
    }
    for mode in 0..4 {
        let (secrets, _) = secrets(0);
        let mut worker = worker(
            client(move |_, _, _, _| {
                let mut result = success(false);
                match mode {
                    0 => result.upload.upload_id = Some(i64::MAX as u64 + 1),
                    1 => result.upload.fight_log_id = Some(0),
                    2 => result.upload.discord_delivery.sent = 1,
                    _ => result = success(true),
                };
                Ok(result)
            }),
            Some(secrets),
            options(),
        );
        worker.enqueue(request(1, false)).unwrap();
        assert_eq!(outcome(&mut worker, 1), UploadOutcome::Failed);
    }
}

#[test]
fn invalid_configuration_and_requests_cannot_replace_the_previous_configuration() {
    let mut worker = worker(
        client(|_, _, _, _| panic!("must not call")),
        None,
        options(),
    );
    let before = worker.config_snapshot();
    for mode in 0..5 {
        let mut next = options();
        match mode {
            0 => next.api_base_url = "http://donbot.example".into(),
            1 => next.guild_id = "0".into(),
            2 => next.guild_id = "01".into(),
            3 => {
                next.delivery = DonBotDeliveryRoute::ChannelOverride {
                    channel_id: String::new(),
                }
            }
            _ => {
                next.guild_id.clear();
                next.delivery = DonBotDeliveryRoute::GuildDefaults;
            }
        };
        assert_eq!(
            worker.update_config(next),
            Err(DonBotWorkerError::InvalidConfiguration)
        );
        assert_eq!(worker.config_snapshot(), before);
    }
    for mode in 0..4 {
        let mut request = request(1, false);
        match mode {
            0 => request.provider = Provider::Wingman,
            1 => request.attempt = 0,
            2 => request.file.canonical_path.clear(),
            _ => request.intent = UploadIntent::Rechat,
        };
        assert_eq!(worker.enqueue(request), Err(ProviderError::InvalidRequest));
    }
    let mut idle = options();
    idle.guild_id.clear();
    worker.update_config(idle).unwrap();
    assert_eq!(
        worker.enqueue(request(1, false)),
        Err(ProviderError::InvalidRequest)
    );
    assert!(worker.try_take_result().unwrap().is_none());
    assert_eq!(
        worker.update_parallelism(0),
        Err(WorkerError::InvalidParallelism)
    );
}

#[test]
fn aggregate_admission_waits_for_result_consumption_and_loads_each_key_off_thread() {
    let (secrets, loads) = secrets(0);
    let (started, ready) = mpsc::sync_channel(1);
    let (release, wait) = mpsc::sync_channel(1);
    let wait = Mutex::new(wait);
    let secret_access = secrets.clone();
    let mut worker = DonBotAggregateWorker::new(
        client(move |call, options, key, _| {
            assert!(secret_access.try_lock().is_ok());
            let Call::Aggregate(ids) = call else {
                panic!("aggregate route")
            };
            assert_eq!(ids, [11, 22]);
            assert_eq!(options.guild_id, "123");
            assert_eq!(options.api_base_url, "https://donbot.example/service");
            assert!(options.delivery.requested());
            assert!(key.expose() == b"SYNTHETIC-1");
            started.send(()).unwrap();
            wait.lock().unwrap().recv_timeout(WAIT).unwrap();
            Ok(success(true))
        }),
        Some(secrets),
    )
    .unwrap();
    worker.enqueue(aggregate_request(8)).unwrap();
    ready.recv_timeout(WAIT).unwrap();
    assert!(worker.busy());
    assert_eq!(
        worker.enqueue(aggregate_request(9)),
        Err(DonBotAggregateDispatchError::Busy)
    );
    release.send(()).unwrap();
    let deadline = Instant::now() + WAIT;
    while worker.snapshot().results == 0 {
        assert!(Instant::now() < deadline);
        std::thread::yield_now();
    }
    assert!(!worker.busy());
    assert_eq!(
        worker.enqueue(aggregate_request(9)),
        Err(DonBotAggregateDispatchError::Busy)
    );
    let result = worker.try_take_result().unwrap();
    assert_eq!(result.request_id, 8);
    assert_eq!(result.outcome, DonBotAggregateOutcome::Succeeded);
    assert_eq!(result.fight_log_count, 2);
    assert_eq!(result.discord_delivery.unwrap().sent, 1);
    assert_eq!(loads.load(Ordering::Relaxed), 1);
}

#[test]
fn aggregate_transport_status_cancellation_and_post_dispatch_panics_never_retry() {
    let cases = [
        (
            DonBotDisposition::Failed,
            None,
            Some(400),
            DonBotErrorCode::HttpStatus,
            false,
        ),
        (
            DonBotDisposition::Failed,
            None,
            Some(403),
            DonBotErrorCode::HttpStatus,
            false,
        ),
        (
            DonBotDisposition::Retry,
            None,
            Some(408),
            DonBotErrorCode::HttpStatus,
            true,
        ),
        (
            DonBotDisposition::Retry,
            None,
            Some(429),
            DonBotErrorCode::HttpStatus,
            true,
        ),
        (
            DonBotDisposition::Retry,
            None,
            Some(503),
            DonBotErrorCode::HttpStatus,
            true,
        ),
        (
            DonBotDisposition::Failed,
            None,
            Some(200),
            DonBotErrorCode::InvalidResponse,
            true,
        ),
        (
            DonBotDisposition::Failed,
            Some(HttpErrorCode::ProtocolError),
            None,
            DonBotErrorCode::Transport,
            true,
        ),
        (
            DonBotDisposition::Cancelled,
            None,
            None,
            DonBotErrorCode::Cancelled,
            true,
        ),
        (
            DonBotDisposition::Failed,
            None,
            None,
            DonBotErrorCode::Internal,
            true,
        ),
    ];
    for (disposition, http_error, http_status, code, ambiguous) in cases {
        let (secrets, _) = secrets(0);
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let mut worker = DonBotAggregateWorker::new(
            client(move |_, _, _, _| {
                count.fetch_add(1, Ordering::Relaxed);
                Err(DonBotError {
                    code,
                    disposition,
                    http_error,
                    http_status,
                    retry_after: Some(Duration::from_millis(1)),
                })
            }),
            Some(secrets),
        )
        .unwrap();
        worker.enqueue(aggregate_request(7)).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(
            result.outcome,
            if ambiguous {
                DonBotAggregateOutcome::Ambiguous
            } else {
                DonBotAggregateOutcome::Failed
            }
        );
        assert_eq!(result.request_id, 7);
        assert!(result.discord_delivery.is_none());
        assert_eq!(calls.load(Ordering::Relaxed), 1);
        assert!(worker.try_take_result().is_none());
    }
    let (secrets, _) = secrets(0);
    let mut worker = DonBotAggregateWorker::new(
        client(|_, _, _, _| panic!("synthetic private panic")),
        Some(secrets),
    )
    .unwrap();
    worker.enqueue(aggregate_request(1)).unwrap();
    assert_eq!(
        worker.wait_for_result(WAIT).unwrap().outcome,
        DonBotAggregateOutcome::Ambiguous
    );
}

#[test]
fn aggregate_receipts_keep_counts_and_classify_partial_and_ambiguous_delivery() {
    for (receipt, expected) in [
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Sent,
                sent: 100,
                ..Default::default()
            },
            DonBotAggregateOutcome::Succeeded,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: 1,
                failed: 1,
                ..Default::default()
            },
            DonBotAggregateOutcome::Succeeded,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                skipped: 1,
                failed: 1,
                ..Default::default()
            },
            DonBotAggregateOutcome::Failed,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Skipped,
                skipped: 1,
                ..Default::default()
            },
            DonBotAggregateOutcome::Failed,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Failed,
                failed: 1,
                ..Default::default()
            },
            DonBotAggregateOutcome::Failed,
        ),
        (
            DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Partial,
                sent: 1,
                ambiguous: 1,
                ..Default::default()
            },
            DonBotAggregateOutcome::Ambiguous,
        ),
    ] {
        let (secrets, _) = secrets(0);
        let returned = receipt.clone();
        let mut worker = DonBotAggregateWorker::new(
            client(move |_, _, _, _| {
                let mut result = success(true);
                result.upload.discord_delivery = returned.clone();
                Ok(result)
            }),
            Some(secrets),
        )
        .unwrap();
        worker.enqueue(aggregate_request(1)).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(result.outcome, expected);
        assert_eq!(result.discord_delivery, Some(receipt));
    }
    for mode in 0..3 {
        let (secrets, _) = secrets(0);
        let mut worker = DonBotAggregateWorker::new(
            client(move |_, _, _, _| {
                let mut result = success(true);
                match mode {
                    0 => result.count = 3,
                    1 => result.upload.discord_delivery.sent = 101,
                    _ => result = success(false),
                };
                Ok(result)
            }),
            Some(secrets),
        )
        .unwrap();
        worker.enqueue(aggregate_request(1)).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(result.outcome, DonBotAggregateOutcome::Ambiguous);
        assert!(result.discord_delivery.is_none());
    }
}

#[test]
fn aggregate_validation_and_storage_failures_precede_client_dispatch() {
    let mut worker =
        DonBotAggregateWorker::new(client(|_, _, _, _| panic!("must not call")), None).unwrap();
    for mode in 0..8 {
        let mut request = aggregate_request(1);
        match mode {
            0 => request.request_id = 0,
            1 => request.fight_log_ids = vec![1],
            2 => request.fight_log_ids = vec![1, 1],
            3 => request.fight_log_ids = vec![0, 2],
            4 => request.fight_log_ids = (1..=101).collect(),
            5 => request.delivery = DonBotDeliveryRoute::None,
            6 => request.guild_id.clear(),
            _ => request.api_base_url = "http://donbot.example".into(),
        };
        assert_eq!(
            worker.enqueue(request),
            Err(DonBotAggregateDispatchError::InvalidRequest)
        );
    }
    worker.enqueue(aggregate_request(1)).unwrap();
    let result = worker.wait_for_result(WAIT).unwrap();
    assert_eq!(result.outcome, DonBotAggregateOutcome::Failed);
    assert_eq!(
        result.credential_failure,
        Some(UploadFailure::DonBotKeyMissing)
    );
    for mode in 1..4 {
        let (secrets, _) = secrets(mode);
        let mut worker =
            DonBotAggregateWorker::new(client(|_, _, _, _| panic!("must not call")), Some(secrets))
                .unwrap();
        worker.enqueue(aggregate_request(2)).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(result.outcome, DonBotAggregateOutcome::Failed);
        assert_eq!(
            result.credential_failure,
            match mode {
                1 => Some(UploadFailure::DonBotKeyMissing),
                2 => Some(UploadFailure::CredentialLoadFailed),
                _ => None,
            }
        );
    }
}

#[test]
fn both_worker_types_cancel_and_join_active_clients_before_returning() {
    for aggregate in [false, true] {
        let (secrets, _) = secrets(0);
        let (started, ready) = mpsc::sync_channel(1);
        let stopped = Arc::new(AtomicBool::new(false));
        let observed = stopped.clone();
        let client = client(move |_, options, _, stop| {
            started.send(()).unwrap();
            assert!(stop.wait_for_stop(WAIT));
            observed.store(true, Ordering::Release);
            Ok(success(options.delivery.requested()))
        });
        if aggregate {
            let mut worker = DonBotAggregateWorker::new(client, Some(secrets)).unwrap();
            worker.enqueue(aggregate_request(1)).unwrap();
            ready.recv_timeout(WAIT).unwrap();
            worker.cancel_pending();
            assert_eq!(
                worker.enqueue(aggregate_request(2)),
                Err(DonBotAggregateDispatchError::Stopping)
            );
            drop(worker);
        } else {
            let mut worker = worker(client, Some(secrets), options());
            worker.enqueue(request(1, false)).unwrap();
            ready.recv_timeout(WAIT).unwrap();
            worker.enqueue(request(2, true)).unwrap();
            worker.cancel_pending();
            assert_eq!(
                worker.enqueue(request(3, false)),
                Err(ProviderError::Stopping)
            );
            drop(worker);
        }
        assert!(stopped.load(Ordering::Acquire));
        assert!(ready.try_recv().is_err());
    }
}

#[test]
fn explicit_retries_and_reuploads_preserve_cpp_discord_suppression() {
    for intent in [UploadIntent::ManualRetry, UploadIntent::Reupload] {
        let (secrets, _) = secrets(0);
        let mut configured = options();
        configured.delivery = DonBotDeliveryRoute::GuildDefaults;
        let mut worker = worker(
            client(|_, options, _, _| {
                assert_eq!(options.delivery, DonBotDeliveryRoute::None);
                Ok(success(false))
            }),
            Some(secrets),
            configured,
        );
        let mut request = request(1, true);
        request.intent = intent;
        worker.enqueue(request).unwrap();
        assert!(matches!(
            outcome(&mut worker, 1),
            UploadOutcome::Succeeded(UploadSuccess::DonBot(Some(_)))
        ));
    }
}

#[test]
fn poisoned_credential_storage_fails_before_network_for_both_workers() {
    let (secrets, loads) = secrets(0);
    let poisoned = secrets.clone();
    let _ = std::thread::spawn(move || {
        let _lock = poisoned.lock().unwrap();
        panic!("synthetic poison");
    })
    .join();
    let mut upload = worker(
        client(|_, _, _, _| panic!("must not call")),
        Some(secrets.clone()),
        options(),
    );
    upload.enqueue(request(1, false)).unwrap();
    assert_eq!(
        outcome(&mut upload, 1),
        UploadOutcome::FailedWithNotice(UploadFailure::CredentialLoadFailed)
    );
    let mut aggregate =
        DonBotAggregateWorker::new(client(|_, _, _, _| panic!("must not call")), Some(secrets))
            .unwrap();
    aggregate.enqueue(aggregate_request(1)).unwrap();
    let result = aggregate.wait_for_result(WAIT).unwrap();
    assert_eq!(result.outcome, DonBotAggregateOutcome::Failed);
    assert_eq!(
        result.credential_failure,
        Some(UploadFailure::CredentialLoadFailed)
    );
    assert_eq!(loads.load(Ordering::Relaxed), 0);
}

#[test]
fn cancellation_during_key_loading_never_admits_the_client() {
    struct BlockedStore {
        started: mpsc::SyncSender<()>,
        release: mpsc::Receiver<()>,
    }
    impl SecretStore for BlockedStore {
        fn mode(&self) -> SecretProtectionMode {
            SecretProtectionMode::NativeUserScoped
        }
        fn load(&self, _: SecretId) -> Result<SecretValue, SecretStoreError> {
            self.started.send(()).unwrap();
            self.release.recv_timeout(WAIT).unwrap();
            Ok(SecretValue::from_bytes(b"SYNTHETIC".to_vec()))
        }
        fn store(&mut self, _: SecretId, _: &SecretValue) -> Result<(), SecretStoreError> {
            unreachable!()
        }
        fn erase(&mut self, _: SecretId) -> Result<(), SecretStoreError> {
            unreachable!()
        }
    }
    for aggregate in [false, true] {
        let (started, ready) = mpsc::sync_channel(1);
        let (release, wait) = mpsc::sync_channel(1);
        let secrets: Secrets = Arc::new(Mutex::new(Box::new(BlockedStore {
            started,
            release: wait,
        })));
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let client = client(move |_, _, _, _| {
            count.fetch_add(1, Ordering::Relaxed);
            Ok(success(true))
        });
        if aggregate {
            let mut worker = DonBotAggregateWorker::new(client, Some(secrets)).unwrap();
            worker.enqueue(aggregate_request(1)).unwrap();
            ready.recv_timeout(WAIT).unwrap();
            worker.cancel_pending();
            release.send(()).unwrap();
            drop(worker);
        } else {
            let mut worker = worker(client, Some(secrets), options());
            worker.enqueue(request(1, false)).unwrap();
            ready.recv_timeout(WAIT).unwrap();
            worker.cancel_pending();
            release.send(()).unwrap();
            drop(worker);
        }
        assert_eq!(calls.load(Ordering::Relaxed), 0);
    }
}
