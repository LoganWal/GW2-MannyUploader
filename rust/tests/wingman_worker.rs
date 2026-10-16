use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, SystemTime};

const WAIT: Duration = Duration::from_secs(5);

#[test]
fn owned_port_reconfigures_parallelism_and_rejects_mismatched_policy() {
    let mut worker = worker(|_, _| Ok(success(false, false)));
    worker
        .configure(ProviderConfiguration {
            parallelism: 3,
            policy: ProviderPolicy::Wingman,
        })
        .unwrap();
    assert_eq!(worker.snapshot().parallelism, 3);
    for config in [
        ProviderConfiguration {
            parallelism: 0,
            policy: ProviderPolicy::Wingman,
        },
        ProviderConfiguration {
            parallelism: 2,
            policy: ProviderPolicy::DpsReport { detailed_wvw: true },
        },
    ] {
        assert_eq!(
            worker.configure(config),
            Err(ProviderConfigurationError::InvalidConfiguration)
        );
        assert_eq!(worker.snapshot().parallelism, 3);
    }
    worker.cancel_pending();
    assert_eq!(
        worker.configure(ProviderConfiguration {
            parallelism: 1,
            policy: ProviderPolicy::Wingman
        }),
        Err(ProviderConfigurationError::Stopping)
    );
}
enum Call<'a> {
    Direct(&'a LogFileIdentity, &'a EncounterMetadata),
    Import(&'a str),
}
struct Client<F>(F);
impl<F> WingmanUploader for Client<F>
where
    F: Fn(Call<'_>, &StopToken) -> Result<WingmanSuccess, WingmanError> + Send + Sync,
{
    fn upload(
        &self,
        file: &LogFileIdentity,
        metadata: &EncounterMetadata,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError> {
        (self.0)(Call::Direct(file, metadata), stop)
    }
    fn import_permalink(
        &self,
        link: &str,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError> {
        (self.0)(Call::Import(link), stop)
    }
}
fn worker(
    f: impl Fn(Call<'_>, &StopToken) -> Result<WingmanSuccess, WingmanError> + Send + Sync + 'static,
) -> WingmanProviderWorker {
    WingmanProviderWorker::new(
        Arc::new(Client(f)),
        WorkerOptions {
            queue_capacity: 1,
            parallelism: 1,
        },
    )
    .unwrap()
}
fn request(import: bool) -> UploadRequest {
    UploadRequest {
        job_id: UploadJobId::new(42).unwrap(),
        provider: Provider::Wingman,
        attempt: 3,
        file: LogFileIdentity {
            canonical_path: "absent-fixture.zevtc".into(),
            size: 7,
            last_write_time: SystemTime::UNIX_EPOCH,
        },
        metadata: EncounterMetadata {
            boss_id: 123,
            pov_account: "Fixture.1234".into(),
            remaining_health_basis_points: None,
        },
        dps_report_result: import.then(|| DpsReportResult {
            permalink: "https://dps.report/fixture".into(),
            encounter_name: "Boss".into(),
            boss_id: 123,
            mode: String::new(),
            success: true,
        }),
        intent: UploadIntent::Automatic,
    }
}
fn success(duplicate: bool, link: bool) -> WingmanSuccess {
    WingmanSuccess {
        duplicate,
        permalink: link.then(|| format!("{WINGMAN_LOG_URL}fixture")),
    }
}
fn outcome(worker: &mut WingmanProviderWorker) -> UploadOutcome {
    let result = worker
        .wait_for_result(WAIT)
        .expect("worker result should arrive");
    assert_eq!(
        (result.job_id.get(), result.provider, result.attempt),
        (42, Provider::Wingman, 3)
    );
    result.outcome
}

#[test]
fn routing_preserves_same_job_inputs_and_success_notices() {
    for imported in [false, true] {
        for duplicate in [false, true] {
            for link in [false, true] {
                let mut worker = worker(move |call, _| {
                    match call {
                        Call::Direct(file, metadata) => {
                            assert!(!imported);
                            assert_eq!(file, &request(false).file);
                            assert_eq!(metadata, &request(false).metadata);
                        }
                        Call::Import(link) => {
                            assert!(imported);
                            assert_eq!(link, "https://dps.report/fixture");
                        }
                    }
                    Ok(success(duplicate, link))
                });
                assert_eq!(worker.provider(), Provider::Wingman);
                worker.enqueue(request(imported)).unwrap();
                let receipt = UploadSuccess::Wingman(link.then(|| WingmanUploadReceipt {
                    permalink: format!("{WINGMAN_LOG_URL}fixture"),
                }));
                let expected = if duplicate {
                    UploadOutcome::SucceededWithNotice(receipt, UploadNotice::AlreadyPresent)
                } else if imported {
                    UploadOutcome::SucceededWithNotice(receipt, UploadNotice::Queued)
                } else {
                    UploadOutcome::Succeeded(receipt)
                };
                assert_eq!(outcome(&mut worker), expected);
            }
        }
    }
}

#[test]
fn retry_and_failure_mapping_is_bounded_and_contains_panics() {
    for (delay, expected) in [
        (None, 30),
        (Some(0), 30),
        (Some(1), 1),
        (Some(86400), 86400),
        (Some(86401), 30),
    ] {
        let mut worker = worker(move |_, _| {
            Err(WingmanError {
                disposition: WingmanDisposition::Retry,
                retry_after: delay.map(Duration::from_secs),
                ..WingmanError::failed(WingmanErrorCode::Transport)
            })
        });
        worker.enqueue(request(false)).unwrap();
        assert_eq!(
            outcome(&mut worker),
            UploadOutcome::RetryAfter(Duration::from_secs(expected))
        );
    }
    for disposition in [WingmanDisposition::Failed, WingmanDisposition::Cancelled] {
        let mut worker = worker(move |_, _| {
            Err(WingmanError {
                disposition,
                ..WingmanError::failed(WingmanErrorCode::Transport)
            })
        });
        worker.enqueue(request(true)).unwrap();
        assert_eq!(
            outcome(&mut worker),
            if disposition == WingmanDisposition::Cancelled {
                UploadOutcome::Cancelled
            } else {
                UploadOutcome::Failed
            }
        );
    }
    let mut worker = worker(|_, _| panic!("synthetic private provider error"));
    worker.enqueue(request(false)).unwrap();
    assert_eq!(outcome(&mut worker), UploadOutcome::Failed);
}

#[test]
fn invalid_requests_and_wvw_are_rejected_before_dispatch() {
    let mut worker = worker(|_, _| panic!("must not dispatch"));
    for imported in [false, true] {
        for mode in 0..4 {
            let mut request = request(imported);
            match mode {
                0 => request.metadata.boss_id = WVW_BOSS_ID,
                1 => request.provider = Provider::DpsReport,
                2 => request.attempt = 0,
                _ => request.file.canonical_path.clear(),
            }
            assert_eq!(worker.enqueue(request), Err(ProviderError::InvalidRequest));
        }
    }
    assert!(worker.try_take_result().unwrap().is_none());
    assert_eq!(worker.snapshot().active, 0);
    assert_eq!(
        worker.update_parallelism(0),
        Err(WorkerError::InvalidParallelism)
    );
}

#[test]
fn bounded_admission_and_cancellation_join_the_active_client() {
    let (started, ready) = mpsc::sync_channel(1);
    let stopped = Arc::new(AtomicBool::new(false));
    let observed = stopped.clone();
    let calls = Arc::new(Mutex::new(0));
    let count = calls.clone();
    let mut worker = worker(move |_, stop| {
        *count.lock().unwrap() += 1;
        started.send(()).unwrap();
        assert!(stop.wait_for_stop(WAIT));
        observed.store(true, Ordering::Release);
        Ok(success(false, true))
    });
    worker.enqueue(request(true)).unwrap();
    ready.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(false)).unwrap();
    assert_eq!(
        worker.enqueue(request(false)),
        Err(ProviderError::QueueFull)
    );
    worker.cancel_pending();
    assert_eq!(worker.enqueue(request(false)), Err(ProviderError::Stopping));
    drop(worker);
    assert!(stopped.load(Ordering::Acquire));
    assert_eq!(*calls.lock().unwrap(), 1);
}
