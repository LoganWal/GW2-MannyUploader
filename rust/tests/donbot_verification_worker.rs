use manny_uploader::application::*;
use manny_uploader::providers::*;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, mpsc};
use std::time::Duration;

const WAIT: Duration = Duration::from_secs(5);
struct Client<F>(F);
impl<F> DonBotIdentityClient for Client<F>
where
    F: Fn(&str, &SecretValue, &StopToken) -> Result<DonBotVerification, DonBotError> + Send + Sync,
{
    fn verify(
        &self,
        base: &str,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotVerification, DonBotError> {
        (self.0)(base, key, stop)
    }
}
fn client(
    f: impl Fn(&str, &SecretValue, &StopToken) -> Result<DonBotVerification, DonBotError>
    + Send
    + Sync
    + 'static,
) -> Arc<dyn DonBotIdentityClient> {
    Arc::new(Client(f))
}
fn identity() -> DonBotVerification {
    DonBotVerification {
        account_name: "Fixture.1234".into(),
        discord_summary_delivery_v1: false,
        discord_aggregate_delivery_v1: false,
        guilds: vec![],
    }
}
fn request(id: u64) -> DonBotVerificationRequest {
    DonBotVerificationRequest {
        request_id: id,
        api_base_url: "https://donbot.example/service///".into(),
        api_key: SecretValue::from_bytes(b"SYNTHETIC-KEY".to_vec()),
    }
}

#[test]
fn verified_key_moves_to_the_correlated_result_without_copying_or_persistence() {
    let request = request(42);
    let original = request.api_key.expose().as_ptr() as usize;
    let mut worker = DonBotVerificationWorker::new(
        client(move |base, key, _| {
            assert_eq!(base, "https://donbot.example/service");
            assert_eq!(key.expose().as_ptr() as usize, original);
            Ok(identity())
        }),
        2,
    )
    .unwrap();
    worker.enqueue(request).unwrap();
    let result = worker.wait_for_result(WAIT).unwrap();
    assert_eq!(result.request_id, 42);
    assert!(!format!("{result:?}").contains("SYNTHETIC"));
    let success = result.verification.unwrap();
    assert_eq!(success.identity, identity());
    assert_eq!(success.api_key.expose().as_ptr() as usize, original);
    assert!(success.api_key.expose() == b"SYNTHETIC-KEY");
}

#[test]
fn failed_cancelled_retryable_and_panicking_clients_return_no_key_and_never_retry() {
    for mode in 0..4 {
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let mut worker = DonBotVerificationWorker::new(
            client(move |_, _, _| {
                count.fetch_add(1, Ordering::Relaxed);
                if mode == 3 {
                    panic!("synthetic private error");
                }
                Err(DonBotError {
                    disposition: match mode {
                        0 => DonBotDisposition::Failed,
                        1 => DonBotDisposition::Cancelled,
                        _ => DonBotDisposition::Retry,
                    },
                    retry_after: Some(Duration::from_millis(1)),
                    ..DonBotError::failed(DonBotErrorCode::Transport)
                })
            }),
            1,
        )
        .unwrap();
        worker.enqueue(request(7)).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(result.request_id, 7);
        assert_eq!(
            result.verification.unwrap_err(),
            if mode == 1 {
                DonBotVerificationFailure::Cancelled
            } else {
                DonBotVerificationFailure::Failed
            }
        );
        assert_eq!(calls.load(Ordering::Relaxed), 1);
        assert!(worker.try_take_result().is_none());
    }
}

#[test]
fn invalid_admission_is_rejected_before_starting_verification() {
    let client = client(|_, _, _| panic!("must not verify"));
    assert!(matches!(
        DonBotVerificationWorker::new(client.clone(), 0),
        Err(WorkerError::InvalidCapacity)
    ));
    let mut worker = DonBotVerificationWorker::new(client, 1).unwrap();
    for mode in 0..4 {
        let mut request = request(1);
        match mode {
            0 => request.request_id = 0,
            1 => request.api_base_url = "http://donbot.example".into(),
            2 => request.api_key.clear(),
            _ => request.api_key = SecretValue::from_bytes(b"bad key".to_vec()),
        }
        assert_eq!(
            worker.enqueue(request),
            Err(DonBotVerificationDispatchError::InvalidRequest)
        );
    }
    assert!(worker.try_take_result().is_none());
    worker.cancel_pending();
    assert_eq!(
        worker.enqueue(request(1)),
        Err(DonBotVerificationDispatchError::Stopping)
    );
    assert!(!format!("{:?}", request(2)).contains("SYNTHETIC"));
}

#[test]
fn bounded_fifo_and_single_worker_preserve_each_key_during_output_backpressure() {
    let (started, ready) = mpsc::sync_channel(4);
    let (release, wait) = mpsc::sync_channel(4);
    let wait = std::sync::Mutex::new(wait);
    let mut worker = DonBotVerificationWorker::new(
        client(move |_, key, _| {
            started.send(key.expose().to_vec()).unwrap();
            wait.lock().unwrap().recv_timeout(WAIT).unwrap();
            Ok(identity())
        }),
        1,
    )
    .unwrap();
    worker.enqueue(request(1)).unwrap();
    assert_eq!(ready.recv_timeout(WAIT).unwrap(), b"SYNTHETIC-KEY");
    let mut second = request(2);
    second.api_key = SecretValue::from_bytes(b"SECOND-KEY".to_vec());
    worker.enqueue(second).unwrap();
    assert_eq!(
        worker.enqueue(request(3)),
        Err(DonBotVerificationDispatchError::QueueFull)
    );
    release.send(()).unwrap();
    assert_eq!(ready.recv_timeout(WAIT).unwrap(), b"SECOND-KEY");
    worker.enqueue(request(3)).unwrap();
    release.send(()).unwrap();
    assert_eq!(worker.snapshot().owned_threads, 1);
    assert_eq!(worker.snapshot().parallelism, 1);
    let first = worker.wait_for_result(WAIT).unwrap();
    assert_eq!(first.request_id, 1);
    assert!(first.verification.unwrap().api_key.expose() == b"SYNTHETIC-KEY");
    assert_eq!(worker.wait_for_result(WAIT).unwrap().request_id, 2);
    ready.recv_timeout(WAIT).unwrap();
    release.send(()).unwrap();
    assert_eq!(worker.wait_for_result(WAIT).unwrap().request_id, 3);
}

#[test]
fn cancellation_clears_pending_keys_and_joins_the_client_before_drop_returns() {
    let stopped = Arc::new(AtomicBool::new(false));
    let observed = stopped.clone();
    let (started, ready) = mpsc::sync_channel(1);
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let mut worker = DonBotVerificationWorker::new(
        client(move |_, _, stop| {
            count.fetch_add(1, Ordering::Relaxed);
            started.send(()).unwrap();
            assert!(stop.wait_for_stop(WAIT));
            observed.store(true, Ordering::Release);
            Ok(identity())
        }),
        2,
    )
    .unwrap();
    worker.enqueue(request(1)).unwrap();
    ready.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(2)).unwrap();
    worker.cancel_pending();
    worker.cancel_pending();
    assert!(worker.try_take_result().is_none());
    drop(worker);
    assert!(stopped.load(Ordering::Acquire));
    assert_eq!(calls.load(Ordering::Relaxed), 1);
}
