use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, SyncSender};
use std::time::{Duration, Instant, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;

const TIMEOUT: Duration = Duration::from_secs(5);

struct Call {
    request: UploadRequest,
    release: SyncSender<UploadOutcome>,
}

fn controlled(
    provider: Provider,
    capacity: usize,
    parallelism: usize,
) -> (AsyncUploadWorker, Receiver<Call>) {
    let (started, calls) = mpsc::sync_channel(32);
    let worker = AsyncUploadWorker::new(
        provider,
        move |request: &UploadRequest, _: &StopToken| {
            let (release, response) = mpsc::sync_channel(1);
            started
                .send(Call {
                    request: request.clone(),
                    release,
                })
                .unwrap();
            // A bounded test wait ensures a failed assertion cannot strand joined destruction.
            response
                .recv_timeout(TIMEOUT)
                .unwrap_or(UploadOutcome::Cancelled)
        },
        WorkerOptions {
            queue_capacity: capacity,
            parallelism,
        },
    )
    .unwrap();
    (worker, calls)
}

fn request(id: u64, provider: Provider) -> UploadRequest {
    UploadRequest {
        job_id: UploadJobId::new(id).unwrap(),
        provider,
        attempt: 3,
        file: LogFileIdentity {
            canonical_path: "logs/example.zevtc".into(),
            size: 123,
            last_write_time: SystemTime::UNIX_EPOCH,
        },
        metadata: EncounterMetadata {
            boss_id: 123,
            pov_account: "Player.1234".into(),
            remaining_health_basis_points: None,
        },
        dps_report_result: None,
        intent: UploadIntent::Reupload,
    }
}

fn next(calls: &Receiver<Call>) -> Call {
    calls.recv_timeout(TIMEOUT).expect("processor should start")
}
fn result(worker: &mut AsyncUploadWorker) -> UploadResult {
    worker
        .wait_for_result(TIMEOUT)
        .expect("result should arrive")
}
fn immediate(_: &UploadRequest, _: &StopToken) -> UploadOutcome {
    UploadOutcome::Skipped
}

#[test]
fn construction_validates_bounds_before_starting_threads() {
    assert_eq!(
        WorkerOptions::default(),
        WorkerOptions {
            queue_capacity: 8,
            parallelism: 1
        }
    );
    for (options, error) in [
        (
            WorkerOptions {
                queue_capacity: 0,
                parallelism: 1,
            },
            WorkerError::InvalidCapacity,
        ),
        (
            WorkerOptions {
                queue_capacity: 1,
                parallelism: 0,
            },
            WorkerError::InvalidParallelism,
        ),
        (
            WorkerOptions {
                queue_capacity: 1,
                parallelism: 33,
            },
            WorkerError::InvalidParallelism,
        ),
        (
            WorkerOptions {
                queue_capacity: usize::MAX,
                parallelism: 1,
            },
            WorkerError::AllocationFailed,
        ),
    ] {
        assert!(
            matches!(AsyncUploadWorker::new(Provider::DpsReport, immediate, options), Err(actual) if actual == error)
        );
    }
}

#[test]
fn invalid_requests_are_rejected_without_admission() {
    let mut worker =
        AsyncUploadWorker::new(Provider::DpsReport, immediate, WorkerOptions::default()).unwrap();
    let mut zero_attempt = request(1, Provider::DpsReport);
    zero_attempt.attempt = 0;
    let mut empty_path = request(1, Provider::DpsReport);
    empty_path.file.canonical_path.clear();
    for invalid in [zero_attempt, empty_path, request(1, Provider::Wingman)] {
        assert_eq!(worker.enqueue(invalid), Err(ProviderError::InvalidRequest));
    }
    assert_eq!(worker.snapshot().pending, 0);
    assert_eq!(worker.snapshot().active, 0);
    assert_eq!(worker.provider(), Provider::DpsReport);
}

#[test]
fn fifo_requests_preserve_identity_attempt_intent_and_unicode() {
    let (mut worker, calls) = controlled(Provider::DonBot, 3, 1);
    let mut first = request(1, Provider::DonBot);
    first.file.canonical_path = "logs/\u{96ea}/\u{00e9}.zevtc".into();
    worker.enqueue(first.clone()).unwrap();
    let call = next(&calls);
    assert_eq!(call.request, first);
    worker.enqueue(request(2, Provider::DonBot)).unwrap();
    worker.enqueue(request(3, Provider::DonBot)).unwrap();
    call.release.send(UploadOutcome::Skipped).unwrap();
    for id in 1..=3 {
        let completed = result(&mut worker);
        assert_eq!(completed.job_id.get(), id);
        assert_eq!(completed.provider, Provider::DonBot);
        assert_eq!(completed.attempt, 3);
        assert_eq!(completed.outcome, UploadOutcome::Skipped);
        if id < 3 {
            let call = next(&calls);
            assert_eq!(call.request.job_id.get(), id + 1);
            call.release.send(UploadOutcome::Skipped).unwrap();
        }
    }
}

#[test]
fn input_capacity_and_result_backpressure_bound_admission() {
    let (mut worker, calls) = controlled(Provider::Wingman, 1, 1);
    worker.enqueue(request(1, Provider::Wingman)).unwrap();
    let first = next(&calls);
    worker.enqueue(request(2, Provider::Wingman)).unwrap();
    assert_eq!(
        worker.enqueue(request(3, Provider::Wingman)),
        Err(ProviderError::QueueFull)
    );
    first.release.send(UploadOutcome::Skipped).unwrap();
    let second = next(&calls);
    second.release.send(UploadOutcome::Skipped).unwrap();
    worker.enqueue(request(3, Provider::Wingman)).unwrap();
    let snapshot = worker.snapshot();
    assert_eq!(
        (snapshot.results, snapshot.active, snapshot.pending),
        (1, 1, 1)
    );
    assert_eq!(
        worker.enqueue(request(4, Provider::Wingman)),
        Err(ProviderError::QueueFull)
    );
    assert_eq!(result(&mut worker).job_id.get(), 1);
    let third = next(&calls);
    assert_eq!(third.request.job_id.get(), 3);
    assert_eq!(result(&mut worker).job_id.get(), 2);
    third.release.send(UploadOutcome::Skipped).unwrap();
    assert_eq!(result(&mut worker).job_id.get(), 3);
}

#[test]
fn parallel_results_are_published_in_completion_order() {
    let (mut worker, calls) = controlled(Provider::Wingman, 3, 3);
    for id in 1..=3 {
        worker.enqueue(request(id, Provider::Wingman)).unwrap();
    }
    let mut active: Vec<_> = (0..3).map(|_| next(&calls)).collect();
    active.sort_by_key(|call| call.request.job_id.get());
    for call in active.into_iter().rev() {
        call.release.send(UploadOutcome::Skipped).unwrap();
        assert_eq!(result(&mut worker).job_id, call.request.job_id);
    }
}

#[test]
fn raising_parallelism_admits_more_work_and_lowering_waits_for_active_work() {
    let (mut worker, calls) = controlled(Provider::Wingman, 4, 1);
    worker.enqueue(request(1, Provider::Wingman)).unwrap();
    let first = next(&calls);
    for id in 2..=4 {
        worker.enqueue(request(id, Provider::Wingman)).unwrap();
    }
    worker.update_parallelism(3).unwrap();
    let second = next(&calls);
    let third = next(&calls);
    assert_eq!(worker.snapshot().active, 3);
    worker.update_parallelism(1).unwrap();
    assert_eq!(worker.snapshot().owned_threads, 3);
    for (call, active_count) in [(first, 2), (second, 1)] {
        call.release.send(UploadOutcome::Skipped).unwrap();
        result(&mut worker);
        assert_eq!(worker.snapshot().active, active_count);
        assert_eq!(worker.snapshot().pending, 1);
    }
    third.release.send(UploadOutcome::Skipped).unwrap();
    result(&mut worker);
    let fourth = next(&calls);
    assert_eq!(fourth.request.job_id.get(), 4);
    for invalid in [0, 33] {
        assert_eq!(
            worker.update_parallelism(invalid),
            Err(WorkerError::InvalidParallelism)
        );
        assert_eq!(worker.snapshot().parallelism, 1);
    }
    worker.update_parallelism(3).unwrap();
    assert_eq!(worker.snapshot().owned_threads, 3);
    fourth.release.send(UploadOutcome::Skipped).unwrap();
    result(&mut worker);
}

#[test]
fn providers_have_independent_admission_and_backpressure() {
    let (mut wingman, wingman_calls) = controlled(Provider::Wingman, 1, 1);
    let (mut donbot, donbot_calls) = controlled(Provider::DonBot, 1, 1);
    wingman.enqueue(request(1, Provider::Wingman)).unwrap();
    let first = next(&wingman_calls);
    wingman.enqueue(request(2, Provider::Wingman)).unwrap();
    first.release.send(UploadOutcome::Skipped).unwrap();
    let second = next(&wingman_calls);
    second.release.send(UploadOutcome::Skipped).unwrap();
    assert_eq!(wingman.snapshot().results, 1);
    donbot.enqueue(request(3, Provider::DonBot)).unwrap();
    next(&donbot_calls)
        .release
        .send(UploadOutcome::Skipped)
        .unwrap();
    assert_eq!(result(&mut donbot).job_id.get(), 3);
    wingman.cancel_pending();
    assert!(wingman.try_take_result().unwrap().is_none());
}

#[test]
fn outcomes_are_passed_through_and_panics_become_failed_attempts() {
    let outcomes = [
        UploadOutcome::Skipped,
        UploadOutcome::RetryAfter(Duration::from_secs(30)),
        UploadOutcome::Ambiguous,
        UploadOutcome::Cancelled,
    ];
    for outcome in outcomes {
        let expected = outcome.clone();
        let mut worker = AsyncUploadWorker::new(
            Provider::Twitch,
            move |_: &UploadRequest, _: &StopToken| outcome.clone(),
            WorkerOptions::default(),
        )
        .unwrap();
        worker.enqueue(request(1, Provider::Twitch)).unwrap();
        assert_eq!(result(&mut worker).outcome, expected);
    }
    let mut worker = AsyncUploadWorker::new(
        Provider::DpsReport,
        |request: &UploadRequest, _: &StopToken| {
            if request.job_id.get() == 1 {
                panic!("synthetic processor failure");
            }
            UploadOutcome::Skipped
        },
        WorkerOptions::default(),
    )
    .unwrap();
    worker.enqueue(request(1, Provider::DpsReport)).unwrap();
    worker.enqueue(request(2, Provider::DpsReport)).unwrap();
    let failure = result(&mut worker);
    assert_eq!((failure.job_id.get(), failure.attempt), (1, 3));
    assert_eq!(failure.outcome, UploadOutcome::Failed);
    assert_eq!(result(&mut worker).outcome, UploadOutcome::Skipped);
}

#[test]
fn cancellation_interrupts_processors_discards_pending_work_and_rejects_new_work() {
    let (started, calls) = mpsc::sync_channel(2);
    let observed_stop = Arc::new(AtomicBool::new(false));
    let observed = observed_stop.clone();
    let mut worker = AsyncUploadWorker::new(
        Provider::DpsReport,
        move |_: &UploadRequest, stop: &StopToken| {
            started.send(()).unwrap();
            observed.store(stop.wait_for_stop(TIMEOUT), Ordering::Release);
            UploadOutcome::Skipped
        },
        WorkerOptions::default(),
    )
    .unwrap();
    worker.enqueue(request(1, Provider::DpsReport)).unwrap();
    calls.recv_timeout(TIMEOUT).unwrap();
    worker.enqueue(request(2, Provider::DpsReport)).unwrap();
    worker.cancel_pending();
    worker.cancel_pending();
    assert!(worker.snapshot().stopping);
    assert_eq!(worker.snapshot().pending, 0);
    assert!(worker.wait_for_result(Duration::ZERO).is_none());
    assert_eq!(
        worker.enqueue(request(3, Provider::DpsReport)),
        Err(ProviderError::Stopping)
    );
    assert_eq!(worker.update_parallelism(2), Err(WorkerError::Stopping));
    drop(worker);
    assert!(observed_stop.load(Ordering::Acquire));
    assert!(calls.try_recv().is_err());
}

#[test]
fn cancellation_wakes_publishers_blocked_by_a_full_result_queue() {
    let (mut worker, calls) = controlled(Provider::Wingman, 1, 1);
    worker.enqueue(request(1, Provider::Wingman)).unwrap();
    let first = next(&calls);
    worker.enqueue(request(2, Provider::Wingman)).unwrap();
    first.release.send(UploadOutcome::Skipped).unwrap();
    let second = next(&calls);
    second.release.send(UploadOutcome::Skipped).unwrap();
    assert_eq!(worker.snapshot().results, 1);
    worker.cancel_pending();
    assert_eq!(worker.snapshot().results, 0);
    assert!(worker.try_take_result().unwrap().is_none());
    drop(worker);
}

struct DropProbe(Arc<AtomicBool>);
impl Drop for DropProbe {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Release);
    }
}

#[test]
fn destruction_joins_before_releasing_the_processor() {
    let (started, calls) = mpsc::sync_channel(1);
    let exited = Arc::new(AtomicBool::new(false));
    let released = Arc::new(AtomicBool::new(false));
    let processor_exited = exited.clone();
    let probe = DropProbe(released.clone());
    let mut worker = AsyncUploadWorker::new(
        Provider::Wingman,
        move |_: &UploadRequest, stop: &StopToken| {
            let _keep_alive = &probe;
            started.send(()).unwrap();
            assert!(stop.wait_for_stop(TIMEOUT));
            processor_exited.store(true, Ordering::Release);
            UploadOutcome::Cancelled
        },
        WorkerOptions::default(),
    )
    .unwrap();
    worker.enqueue(request(1, Provider::Wingman)).unwrap();
    calls.recv_timeout(TIMEOUT).unwrap();
    assert!(!released.load(Ordering::Acquire));
    drop(worker);
    assert!(exited.load(Ordering::Acquire));
    assert!(released.load(Ordering::Acquire));
}

struct TestClock;
impl Clock for TestClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}

#[test]
fn coordinator_drop_cancels_and_joins_a_real_provider_pool() {
    let (started, calls) = mpsc::sync_channel(1);
    let stopped = Arc::new(AtomicBool::new(false));
    let observed = stopped.clone();
    let worker = AsyncUploadWorker::new(
        Provider::DpsReport,
        move |_: &UploadRequest, stop: &StopToken| {
            started.send(()).unwrap();
            observed.store(stop.wait_for_stop(TIMEOUT), Ordering::Release);
            UploadOutcome::Cancelled
        },
        WorkerOptions::default(),
    )
    .unwrap();
    let mut coordinator = UploadCoordinator::new(TestClock, vec![Box::new(worker)], 5).unwrap();
    let request = request(1, Provider::DpsReport);
    let id = coordinator
        .add_pending(
            request.file,
            ProviderSelection {
                dps_report: true,
                ..Default::default()
            },
        )
        .unwrap();
    coordinator.set_metadata(id, request.metadata).unwrap();
    calls.recv_timeout(TIMEOUT).unwrap();
    drop(coordinator);
    assert!(stopped.load(Ordering::Acquire));
}

#[test]
fn stop_tokens_observe_cancellation_before_and_during_waits() {
    let source = CancellationSource::default();
    let token = source.token();
    assert!(!token.is_stop_requested());
    assert!(!token.wait_for_stop(Duration::ZERO));
    let (ready, receiver) = mpsc::sync_channel(1);
    let waiter = std::thread::spawn(move || {
        ready.send(()).unwrap();
        token.wait_for_stop(TIMEOUT)
    });
    receiver.recv_timeout(TIMEOUT).unwrap();
    source.request_stop();
    source.request_stop();
    assert!(waiter.join().unwrap());
    assert!(source.token().wait_for_stop(Duration::ZERO));
}

#[test]
fn cancellation_reaches_every_active_processor_in_the_pool() {
    use std::sync::atomic::AtomicUsize;
    let (started, calls) = mpsc::sync_channel(3);
    let cancelled = Arc::new(AtomicUsize::new(0));
    let observed = cancelled.clone();
    let mut worker = AsyncUploadWorker::new(
        Provider::Wingman,
        move |_: &UploadRequest, stop: &StopToken| {
            started.send(()).unwrap();
            if stop.wait_for_stop(TIMEOUT) {
                observed.fetch_add(1, Ordering::Release);
            }
            UploadOutcome::Cancelled
        },
        WorkerOptions {
            queue_capacity: 3,
            parallelism: 3,
        },
    )
    .unwrap();
    for id in 1..=3 {
        worker.enqueue(request(id, Provider::Wingman)).unwrap();
    }
    for _ in 0..3 {
        calls.recv_timeout(TIMEOUT).unwrap();
    }
    worker.enqueue(request(4, Provider::Wingman)).unwrap();
    drop(worker);
    assert_eq!(cancelled.load(Ordering::Acquire), 3);
    assert!(calls.try_recv().is_err());
}
