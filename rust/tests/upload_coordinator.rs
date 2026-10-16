use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::*;

#[derive(Clone)]
struct FakeClock {
    origin: Instant,
    seconds: Arc<AtomicU64>,
}

impl FakeClock {
    fn new() -> Self {
        Self {
            origin: Instant::now(),
            seconds: Arc::new(AtomicU64::new(0)),
        }
    }

    fn advance(&self, seconds: u64) {
        self.seconds.fetch_add(seconds, Ordering::Relaxed);
    }
}

impl Clock for FakeClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }
    fn steady_now(&self) -> Instant {
        self.origin + Duration::from_secs(self.seconds.load(Ordering::Relaxed))
    }
}

struct FakeState {
    requests: Vec<UploadRequest>,
    results: VecDeque<UploadResult>,
    capacity: usize,
    cancellations: usize,
    panic_enqueue: bool,
    panic_poll: bool,
    panic_cancel: bool,
}

impl Default for FakeState {
    fn default() -> Self {
        Self {
            requests: Vec::new(),
            results: VecDeque::new(),
            capacity: 32,
            cancellations: 0,
            panic_enqueue: false,
            panic_poll: false,
            panic_cancel: false,
        }
    }
}

struct FakeProvider {
    provider: Provider,
    state: Arc<Mutex<FakeState>>,
}

impl UploadProvider for FakeProvider {
    fn provider(&self) -> Provider {
        self.provider
    }

    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        let mut state = self.state.lock().unwrap();
        if state.panic_enqueue {
            drop(state);
            panic!("synthetic enqueue failure");
        }
        if state.requests.len() >= state.capacity {
            return Err(ProviderError::QueueFull);
        }
        assert_eq!(request.provider, self.provider);
        assert_ne!(request.attempt, 0);
        state.requests.push(request);
        Ok(())
    }

    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        let mut state = self.state.lock().unwrap();
        if state.panic_poll {
            drop(state);
            panic!("synthetic polling failure");
        }
        Ok(state.results.pop_front())
    }

    fn cancel_pending(&mut self) {
        let mut state = self.state.lock().unwrap();
        state.cancellations += 1;
        state.results.clear();
        if state.panic_cancel {
            drop(state);
            panic!("synthetic cancellation failure");
        }
    }
}

struct Fixture {
    coordinator: UploadCoordinator<FakeClock>,
    clock: FakeClock,
    states: [Arc<Mutex<FakeState>>; 4],
}

impl Fixture {
    fn new(limit: usize) -> Self {
        let clock = FakeClock::new();
        let states: [_; 4] = std::array::from_fn(|_| Arc::new(Mutex::new(FakeState::default())));
        let providers = Provider::ALL
            .into_iter()
            .map(|provider| {
                Box::new(FakeProvider {
                    provider,
                    state: states[provider as usize].clone(),
                }) as Box<dyn UploadProvider>
            })
            .collect();
        Self {
            coordinator: UploadCoordinator::new(clock.clone(), providers, limit).unwrap(),
            clock,
            states,
        }
    }

    fn start(&mut self, selection: ProviderSelection, boss_id: u16) -> UploadJobId {
        let id = self.coordinator.add_pending(file(), selection).unwrap();
        self.coordinator
            .set_metadata(id, metadata(boss_id))
            .unwrap();
        id
    }

    fn count(&self, provider: Provider) -> usize {
        self.states[provider as usize]
            .lock()
            .unwrap()
            .requests
            .len()
    }

    fn last(&self, provider: Provider) -> UploadRequest {
        self.states[provider as usize]
            .lock()
            .unwrap()
            .requests
            .last()
            .unwrap()
            .clone()
    }

    fn result(&self, provider: Provider, outcome: UploadOutcome) -> UploadResult {
        let request = self.last(provider);
        UploadResult {
            job_id: request.job_id,
            provider,
            attempt: request.attempt,
            outcome,
        }
    }

    fn complete(&mut self, provider: Provider, outcome: UploadOutcome) {
        self.coordinator
            .apply_result(self.result(provider, outcome))
            .unwrap();
    }

    fn state(&self, id: UploadJobId, provider: Provider) -> ProviderState {
        self.coordinator
            .snapshots()
            .into_iter()
            .find(|snapshot| snapshot.id == id)
            .unwrap()
            .record
            .providers[provider as usize]
            .state
    }

    fn queue(&self, source: Provider, result: UploadResult) {
        self.states[source as usize]
            .lock()
            .unwrap()
            .results
            .push_back(result);
    }
}

fn file() -> LogFileIdentity {
    LogFileIdentity {
        canonical_path: "logs/example.zevtc".into(),
        size: 100,
        last_write_time: SystemTime::UNIX_EPOCH,
    }
}

fn metadata(boss_id: u16) -> EncounterMetadata {
    EncounterMetadata {
        boss_id,
        pov_account: "Player.1234".into(),
        remaining_health_basis_points: Some(0),
    }
}

fn all() -> ProviderSelection {
    ProviderSelection {
        dps_report: true,
        wingman: true,
        donbot: true,
        twitch: true,
    }
}

fn direct() -> ProviderSelection {
    ProviderSelection {
        wingman: true,
        donbot: true,
        ..ProviderSelection::default()
    }
}

fn report_only() -> ProviderSelection {
    ProviderSelection {
        dps_report: true,
        ..ProviderSelection::default()
    }
}

fn report(suffix: &str) -> UploadOutcome {
    UploadOutcome::Succeeded(UploadSuccess::DpsReport(DpsReportResult {
        permalink: format!("https://dps.report/{suffix}"),
        encounter_name: "Example".into(),
        boss_id: 123,
        mode: "Normal".into(),
        success: true,
    }))
}

#[test]
fn successful_provider_warnings_reach_snapshots_without_blocking_dependents() {
    for warning in [
        UploadWarning::ProviderWarning,
        UploadWarning::InvalidReturnedCredential,
        UploadWarning::CredentialStorageUnavailable,
    ] {
        let mut fixture = Fixture::new(8);
        let id = fixture.start(all(), 123);
        let UploadOutcome::Succeeded(success) = report("warning") else {
            unreachable!()
        };
        fixture.complete(
            Provider::DpsReport,
            UploadOutcome::SucceededWithWarning(success, warning),
        );
        let snapshot = fixture
            .coordinator
            .snapshots()
            .into_iter()
            .find(|row| row.id == id)
            .unwrap();
        assert_eq!(
            snapshot.record.providers[Provider::DpsReport as usize].detail,
            warning.detail()
        );
        assert_eq!(
            snapshot.record.providers[Provider::DpsReport as usize].state,
            ProviderState::Succeeded
        );
        assert!(snapshot.record.dps_report_result.is_some());
        for provider in [Provider::Wingman, Provider::DonBot, Provider::Twitch] {
            assert_eq!(fixture.count(provider), 1);
        }
    }
}

fn sent() -> UploadOutcome {
    UploadOutcome::Succeeded(UploadSuccess::Twitch(TwitchDeliveryReceipt {
        status: TwitchDeliveryStatus::Sent,
        message_id: Some("message-1".into()),
    }))
}

#[test]
fn waits_for_metadata_then_fans_out_the_same_report() {
    let mut fixture = Fixture::new(5);
    let id = fixture.coordinator.add_pending(file(), all()).unwrap();
    assert!(
        Provider::ALL
            .into_iter()
            .all(|provider| fixture.count(provider) == 0)
    );
    fixture.coordinator.set_metadata(id, metadata(123)).unwrap();
    assert_eq!(fixture.count(Provider::DpsReport), 1);
    assert_eq!(fixture.count(Provider::DonBot), 0);
    fixture.complete(Provider::DpsReport, report("first"));
    for provider in [Provider::Wingman, Provider::DonBot, Provider::Twitch] {
        let request = fixture.last(provider);
        assert_eq!(request.job_id, id);
        assert_eq!(
            request.dps_report_result.unwrap().permalink,
            "https://dps.report/first"
        );
        assert_eq!(request.intent, UploadIntent::Automatic);
    }
    let second = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, report("second"));
    assert_ne!(id, second);
    assert_eq!(
        fixture
            .last(Provider::Twitch)
            .dps_report_result
            .unwrap()
            .permalink,
        "https://dps.report/second"
    );
}

#[test]
fn direct_destinations_dispatch_independently_even_if_one_queue_is_full() {
    let mut fixture = Fixture::new(5);
    fixture.states[Provider::Wingman as usize]
        .lock()
        .unwrap()
        .capacity = 0;
    let id = fixture.coordinator.add_pending(file(), direct()).unwrap();
    assert_eq!(
        fixture.coordinator.set_metadata(id, metadata(123)),
        Err(CoordinatorError::Dispatch(ProviderError::QueueFull))
    );
    assert_eq!(fixture.state(id, Provider::Wingman), ProviderState::Failed);
    assert_eq!(fixture.state(id, Provider::DonBot), ProviderState::Active);
    assert!(fixture.last(Provider::DonBot).dps_report_result.is_none());
    assert_eq!(fixture.count(Provider::DpsReport), 0);
}

#[test]
fn wvw_never_dispatches_wingman_in_either_route_or_reupload() {
    for selection in [all(), direct()] {
        let mut fixture = Fixture::new(5);
        let id = fixture.start(selection, WVW_BOSS_ID);
        if selection.dps_report {
            fixture.complete(Provider::DpsReport, report("wvw"));
        }
        assert_eq!(fixture.state(id, Provider::Wingman), ProviderState::Skipped);
        assert_eq!(fixture.count(Provider::Wingman), 0);
        fixture.complete(
            Provider::DonBot,
            UploadOutcome::Succeeded(UploadSuccess::DonBot(None)),
        );
        fixture.coordinator.reupload(id).unwrap();
        fixture.complete(Provider::DpsReport, report("wvw-again"));
        assert_eq!(fixture.count(Provider::Wingman), 0);
        assert_eq!(
            fixture.last(Provider::DonBot).intent,
            UploadIntent::Reupload
        );
    }
}

#[test]
fn failed_report_skips_dependencies_and_manual_retry_rearms_them() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, UploadOutcome::Failed);
    for provider in [Provider::Wingman, Provider::DonBot, Provider::Twitch] {
        assert_eq!(fixture.state(id, provider), ProviderState::Skipped);
        assert_eq!(fixture.count(provider), 0);
    }
    fixture
        .coordinator
        .retry_failed(id, Provider::DpsReport)
        .unwrap();
    assert_eq!(
        fixture.last(Provider::DpsReport).intent,
        UploadIntent::ManualRetry
    );
    fixture.complete(Provider::DpsReport, report("recovered"));
    assert_eq!(fixture.state(id, Provider::Twitch), ProviderState::Active);
}

#[test]
fn skipped_and_cancelled_reports_settle_waiting_dependencies() {
    for (outcome, expected) in [
        (UploadOutcome::Skipped, ProviderState::Skipped),
        (UploadOutcome::Cancelled, ProviderState::Cancelled),
    ] {
        let mut fixture = Fixture::new(5);
        let id = fixture.start(all(), 123);
        fixture.complete(Provider::DpsReport, outcome);
        for provider in [Provider::Wingman, Provider::DonBot, Provider::Twitch] {
            assert_eq!(fixture.state(id, provider), expected);
        }
    }
}

#[test]
fn retries_use_monotonic_deadlines_and_reject_stale_attempts() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    let old_result = fixture.result(Provider::DpsReport, report("stale"));
    fixture.complete(
        Provider::DpsReport,
        UploadOutcome::RetryAfter(Duration::from_secs(10)),
    );
    fixture.clock.advance(9);
    assert_eq!(fixture.coordinator.dispatch_due_retries(4), 0);
    fixture.clock.advance(1);
    assert_eq!(fixture.coordinator.dispatch_due_retries(0), 0);
    assert_eq!(fixture.coordinator.dispatch_due_retries(1), 1);
    assert_eq!(fixture.last(Provider::DpsReport).attempt, 2);
    assert_eq!(
        fixture.coordinator.apply_result(old_result),
        Err(CoordinatorError::UnexpectedResult)
    );
    assert_eq!(
        fixture.state(id, Provider::DpsReport),
        ProviderState::Active
    );
    let success = fixture.result(Provider::DpsReport, report("current"));
    fixture.coordinator.apply_result(success.clone()).unwrap();
    assert_eq!(
        fixture.coordinator.apply_result(success),
        Err(CoordinatorError::UnexpectedResult)
    );
    assert_eq!(fixture.count(Provider::Twitch), 1);
}

#[test]
fn invalid_results_leave_the_entire_job_unchanged() {
    let mut fixture = Fixture::new(5);
    fixture.start(all(), 123);
    for outcome in [
        UploadOutcome::RetryAfter(Duration::ZERO),
        UploadOutcome::RetryAfter(Duration::from_secs(86401)),
        UploadOutcome::Succeeded(UploadSuccess::Wingman(None)),
        UploadOutcome::Succeeded(UploadSuccess::DpsReport(DpsReportResult {
            permalink: String::new(),
            encounter_name: String::new(),
            boss_id: 0,
            mode: String::new(),
            success: false,
        })),
    ] {
        let before = fixture.coordinator.snapshots();
        assert!(
            fixture
                .coordinator
                .apply_result(fixture.result(Provider::DpsReport, outcome))
                .is_err()
        );
        assert_eq!(fixture.coordinator.snapshots(), before);
    }
    fixture.complete(Provider::DpsReport, report("ready"));
    let before = fixture.coordinator.snapshots();
    let invalid = UploadOutcome::Succeeded(UploadSuccess::Twitch(TwitchDeliveryReceipt {
        status: TwitchDeliveryStatus::AutoMod,
        message_id: None,
    }));
    assert_eq!(
        fixture
            .coordinator
            .apply_result(fixture.result(Provider::Twitch, invalid)),
        Err(CoordinatorError::Domain(JobError::InvalidTwitchDelivery))
    );
    assert_eq!(fixture.coordinator.snapshots(), before);
    fixture.complete(Provider::Twitch, sent());
}

#[test]
fn ambiguous_chat_waits_for_explicit_rechat() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, report("ready"));
    fixture.complete(Provider::Twitch, UploadOutcome::Ambiguous);
    fixture.clock.advance(86400);
    assert_eq!(fixture.coordinator.dispatch_due_retries(100), 0);
    assert_eq!(fixture.state(id, Provider::Twitch), ProviderState::Failed);
    fixture.coordinator.rechat(id).unwrap();
    assert_eq!(fixture.last(Provider::Twitch).intent, UploadIntent::Rechat);
    fixture.complete(Provider::Twitch, sent());
    assert_eq!(
        fixture.coordinator.retry_failed(id, Provider::Twitch),
        Err(CoordinatorError::Domain(
            JobError::ManualRetryRequiresFailure
        ))
    );
    fixture.coordinator.rechat(id).unwrap();
    assert!(
        fixture.coordinator.history()[0]
            .twitch_delivery_receipt
            .is_none()
    );
}

#[test]
fn dropped_chat_receipt_is_cleared_on_manual_retry() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, report("ready"));
    fixture.complete(
        Provider::Twitch,
        UploadOutcome::TwitchDropped(TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::AutoMod,
            message_id: None,
        }),
    );
    assert!(
        fixture.coordinator.history()[0]
            .twitch_delivery_receipt
            .is_some()
    );
    fixture
        .coordinator
        .retry_failed(id, Provider::Twitch)
        .unwrap();
    assert!(
        fixture.coordinator.history()[0]
            .twitch_delivery_receipt
            .is_none()
    );
    assert_eq!(
        fixture.last(Provider::Twitch).intent,
        UploadIntent::ManualRetry
    );
    fixture.complete(Provider::Twitch, sent());
}

#[test]
fn reupload_preflights_all_destinations_before_clearing_receipts() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, report("original"));
    let before = fixture.coordinator.snapshots();
    assert_eq!(
        fixture.coordinator.reupload(id),
        Err(CoordinatorError::Domain(JobError::ExplicitDeliveryBusy))
    );
    assert_eq!(fixture.coordinator.snapshots(), before);
    assert_eq!(fixture.count(Provider::DpsReport), 1);
}

#[test]
fn reupload_preserves_intent_across_retries_and_leaves_twitch_untouched() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, report("original"));
    fixture.complete(
        Provider::Wingman,
        UploadOutcome::Succeeded(UploadSuccess::Wingman(Some(WingmanUploadReceipt {
            permalink: "https://gw2wingman.nevermindcreations.de/log/original".into(),
        }))),
    );
    fixture.complete(
        Provider::DonBot,
        UploadOutcome::Succeeded(UploadSuccess::DonBot(Some(DonBotUploadReceipt {
            fight_log_id: Some(12),
            ..Default::default()
        }))),
    );
    fixture.complete(Provider::Twitch, sent());
    let twitch = fixture.coordinator.history()[0]
        .twitch_delivery_receipt
        .clone();
    fixture.coordinator.reupload(id).unwrap();
    let record = &fixture.coordinator.history()[0];
    assert!(record.dps_report_result.is_none());
    assert!(record.wingman_upload_receipt.is_none());
    assert!(record.donbot_upload_receipt.is_none());
    fixture.complete(Provider::DpsReport, UploadOutcome::Failed);
    fixture
        .coordinator
        .retry_failed(id, Provider::DpsReport)
        .unwrap();
    fixture.complete(
        Provider::DpsReport,
        UploadOutcome::RetryAfter(Duration::from_secs(1)),
    );
    fixture.clock.advance(1);
    assert_eq!(fixture.coordinator.dispatch_due_retries(1), 1);
    assert_eq!(
        fixture.last(Provider::DpsReport).intent,
        UploadIntent::Reupload
    );
    fixture.complete(Provider::DpsReport, report("new"));
    fixture.complete(
        Provider::DonBot,
        UploadOutcome::RetryAfter(Duration::from_secs(1)),
    );
    fixture.clock.advance(1);
    fixture.coordinator.dispatch_due_retries(1);
    assert_eq!(
        fixture.last(Provider::DonBot).intent,
        UploadIntent::Reupload
    );
    assert_eq!(
        fixture.last(Provider::Wingman).intent,
        UploadIntent::Reupload
    );
    assert_eq!(fixture.count(Provider::Twitch), 1);
    assert_eq!(
        fixture.coordinator.history()[0].twitch_delivery_receipt,
        twitch
    );
}

#[test]
fn reupload_does_not_rearm_chat_skipped_by_the_original_failure() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(Provider::DpsReport, UploadOutcome::Failed);
    fixture.coordinator.reupload(id).unwrap();
    fixture.complete(Provider::DpsReport, UploadOutcome::Failed);
    fixture
        .coordinator
        .retry_failed(id, Provider::DpsReport)
        .unwrap();
    fixture.complete(Provider::DpsReport, report("new"));
    assert_eq!(fixture.state(id, Provider::Twitch), ProviderState::Skipped);
    assert_eq!(fixture.count(Provider::Twitch), 0);
}

#[test]
fn explicit_reupload_can_enable_previously_disabled_upload_destinations() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(ProviderSelection::default(), 123);
    fixture.coordinator.reupload(id).unwrap();
    fixture.complete(Provider::DpsReport, report("new"));
    assert_eq!(fixture.count(Provider::Wingman), 1);
    assert_eq!(fixture.count(Provider::DonBot), 1);
    assert_eq!(fixture.count(Provider::Twitch), 0);
}

#[test]
fn history_admission_preserves_busy_jobs_and_rejects_without_mutation() {
    let mut fixture = Fixture::new(1);
    let id = fixture.start(report_only(), 123);
    let before = fixture.coordinator.snapshots();
    assert_eq!(
        fixture.coordinator.add_pending(file(), report_only()),
        Err(CoordinatorError::CapacityReached)
    );
    assert_eq!(fixture.coordinator.snapshots(), before);
    fixture.complete(Provider::DpsReport, report("done"));
    let mut invalid = file();
    invalid.canonical_path.clear();
    let before = fixture.coordinator.snapshots();
    assert_eq!(
        fixture.coordinator.add_pending(invalid, report_only()),
        Err(CoordinatorError::Domain(JobError::EmptyPath))
    );
    assert_eq!(fixture.coordinator.snapshots(), before);
    let second = fixture
        .coordinator
        .add_pending(file(), report_only())
        .unwrap();
    assert_eq!(second.get(), id.get() + 1);
    assert_eq!(fixture.coordinator.snapshots().len(), 1);
    assert_eq!(
        fixture.coordinator.reupload(id),
        Err(CoordinatorError::UnknownJob)
    );
}

#[test]
fn shrinking_history_defers_eviction_until_work_settles() {
    let mut fixture = Fixture::new(3);
    fixture.start(report_only(), 123);
    fixture.start(report_only(), 123);
    let third = fixture.start(report_only(), 123);
    fixture.coordinator.set_history_limit(1).unwrap();
    assert_eq!(fixture.coordinator.snapshots().len(), 3);
    fixture.complete(Provider::DpsReport, report("third"));
    assert_eq!(fixture.coordinator.snapshots().len(), 2);
    assert!(
        !fixture
            .coordinator
            .snapshots()
            .iter()
            .any(|snapshot| snapshot.id == third)
    );
    assert_eq!(
        fixture.coordinator.set_history_limit(0),
        Err(CoordinatorError::InvalidHistoryLimit)
    );
    assert_eq!(
        fixture.coordinator.set_history_limit(MAX_HISTORY_LIMIT + 1),
        Err(CoordinatorError::InvalidHistoryLimit)
    );
    fixture.coordinator.cancel_all();
    assert_eq!(fixture.coordinator.snapshots().len(), 1);
}

#[test]
fn restore_is_atomic_normalizes_interrupted_work_and_never_dispatches() {
    let mut original = Fixture::new(5);
    original.start(all(), 123);
    let record = original.coordinator.history().remove(0);
    let mut invalid = record.clone();
    invalid.file.canonical_path.clear();
    let mut restored = Fixture::new(5);
    assert_eq!(
        restored.coordinator.restore(vec![record.clone(), invalid]),
        Err(CoordinatorError::Domain(JobError::EmptyPath))
    );
    assert!(restored.coordinator.snapshots().is_empty());
    restored.coordinator.restore(vec![record]).unwrap();
    assert!(
        restored.coordinator.history()[0]
            .providers
            .iter()
            .all(|status| status.state == ProviderState::Failed)
    );
    assert_eq!(restored.coordinator.dispatch_due_retries(100), 0);
    assert!(
        Provider::ALL
            .into_iter()
            .all(|provider| restored.count(provider) == 0)
    );
    let id = restored.coordinator.snapshots()[0].id;
    let before = restored.coordinator.snapshots();
    assert_eq!(
        restored.coordinator.retry_failed(id, Provider::Wingman),
        Err(CoordinatorError::DependencyUnavailable)
    );
    assert_eq!(restored.coordinator.snapshots(), before);
    assert_eq!(
        restored.coordinator.restore(Vec::new()),
        Err(CoordinatorError::RestoreRequiresEmptyCoordinator)
    );
}

#[test]
fn restore_keeps_the_bounded_tail_and_snapshots_are_owned() {
    let mut original = Fixture::new(5);
    for _ in 0..3 {
        original.start(ProviderSelection::default(), 123);
    }
    let mut records = original.coordinator.history();
    records[0].file.size = 1;
    records[1].file.size = 2;
    records[2].file.size = 3;
    let mut restored = Fixture::new(2);
    restored.coordinator.restore(records).unwrap();
    let mut snapshots = restored.coordinator.snapshots();
    assert_eq!(
        snapshots
            .iter()
            .map(|snapshot| snapshot.record.file.size)
            .collect::<Vec<_>>(),
        vec![2, 3]
    );
    snapshots[0].record.file.size = 999;
    assert_eq!(restored.coordinator.history()[0].file.size, 2);
    assert_eq!(
        restored
            .coordinator
            .add_pending(file(), report_only())
            .unwrap()
            .get(),
        3
    );
}

#[test]
fn result_polling_is_bounded_fair_and_checks_source_identity() {
    let mut fixture = Fixture::new(5);
    fixture.start(direct(), 123);
    let wingman = fixture.result(
        Provider::Wingman,
        UploadOutcome::Succeeded(UploadSuccess::Wingman(None)),
    );
    let donbot = fixture.result(
        Provider::DonBot,
        UploadOutcome::Succeeded(UploadSuccess::DonBot(None)),
    );
    fixture.queue(Provider::DpsReport, wingman.clone());
    fixture.queue(Provider::Wingman, wingman.clone());
    fixture.queue(Provider::Wingman, wingman);
    fixture.queue(Provider::DonBot, donbot);
    assert_eq!(
        fixture.coordinator.drain_results(0),
        DrainSummary::default()
    );
    assert_eq!(
        fixture.coordinator.drain_results(1),
        DrainSummary {
            consumed: 1,
            rejected: 1,
            provider_errors: 0
        }
    );
    assert_eq!(
        fixture.coordinator.drain_results(2),
        DrainSummary {
            consumed: 2,
            rejected: 0,
            provider_errors: 0
        }
    );
    assert_eq!(
        fixture.coordinator.drain_results(100),
        DrainSummary {
            consumed: 1,
            rejected: 1,
            provider_errors: 0
        }
    );
}

#[test]
fn dispatch_panics_settle_failures_and_shutdown_survives_provider_panics() {
    let mut fixture = Fixture::new(5);
    fixture.states[0].lock().unwrap().panic_enqueue = true;
    let id = fixture.coordinator.add_pending(file(), all()).unwrap();
    assert_eq!(
        fixture.coordinator.set_metadata(id, metadata(123)),
        Err(CoordinatorError::Dispatch(ProviderError::Panicked))
    );
    assert_eq!(
        fixture.state(id, Provider::DpsReport),
        ProviderState::Failed
    );
    assert_eq!(fixture.state(id, Provider::Twitch), ProviderState::Skipped);
    fixture.states[0].lock().unwrap().panic_poll = true;
    assert_eq!(fixture.coordinator.drain_results(1).provider_errors, 1);
    fixture.states[0].lock().unwrap().panic_cancel = true;
    fixture.coordinator.cancel_all();
    fixture.coordinator.cancel_all();
    for state in &fixture.states {
        assert_eq!(state.lock().unwrap().cancellations, 1);
    }
}

#[test]
fn shutdown_cancels_all_busy_states_and_rejects_late_work() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.complete(
        Provider::DpsReport,
        UploadOutcome::RetryAfter(Duration::from_secs(1)),
    );
    let late = fixture.result(Provider::DpsReport, report("late"));
    fixture.coordinator.cancel_all();
    assert!(
        fixture.coordinator.history()[0]
            .providers
            .iter()
            .all(|status| status.state == ProviderState::Cancelled)
    );
    assert_eq!(
        fixture.coordinator.add_pending(file(), all()),
        Err(CoordinatorError::ShuttingDown)
    );
    assert_eq!(
        fixture.coordinator.set_metadata(id, metadata(123)),
        Err(CoordinatorError::ShuttingDown)
    );
    assert_eq!(
        fixture.coordinator.apply_result(late),
        Err(CoordinatorError::ShuttingDown)
    );
    assert_eq!(
        fixture.coordinator.reupload(id),
        Err(CoordinatorError::ShuttingDown)
    );
    assert_eq!(
        fixture.coordinator.rechat(id),
        Err(CoordinatorError::ShuttingDown)
    );
    fixture.clock.advance(1);
    assert_eq!(fixture.coordinator.dispatch_due_retries(5), 0);
    assert_eq!(
        fixture.coordinator.drain_results(5),
        DrainSummary::default()
    );
    let states = fixture.states.clone();
    drop(fixture);
    for state in states {
        assert_eq!(state.lock().unwrap().cancellations, 1);
    }
}

#[test]
fn dropping_coordinator_requests_cancellation() {
    let fixture = Fixture::new(5);
    let states = fixture.states.clone();
    drop(fixture);
    for state in states {
        assert_eq!(state.lock().unwrap().cancellations, 1);
    }
}

#[test]
fn metadata_failures_cannot_be_retried_as_uploads() {
    let mut fixture = Fixture::new(5);
    let id = fixture.coordinator.add_pending(file(), all()).unwrap();
    fixture.coordinator.fail_metadata(id, false).unwrap();
    assert_eq!(
        fixture.coordinator.retry_failed(id, Provider::DpsReport),
        Err(CoordinatorError::Domain(
            JobError::EncounterMetadataRequired
        ))
    );
    assert_eq!(
        fixture.coordinator.set_metadata(id, metadata(123)),
        Err(CoordinatorError::MetadataNotPending)
    );
    assert_eq!(fixture.count(Provider::DpsReport), 0);
}

#[test]
fn provider_availability_and_identity_are_validated_before_admission() {
    let clock = FakeClock::new();
    assert!(matches!(
        UploadCoordinator::new(clock.clone(), Vec::new(), 0),
        Err(CoordinatorError::InvalidHistoryLimit)
    ));
    let providers = (0..2)
        .map(|_| {
            Box::new(FakeProvider {
                provider: Provider::DpsReport,
                state: Arc::new(Mutex::new(FakeState::default())),
            }) as Box<dyn UploadProvider>
        })
        .collect();
    assert!(matches!(
        UploadCoordinator::new(clock.clone(), providers, 5),
        Err(CoordinatorError::DuplicateProvider)
    ));
    let mut coordinator = UploadCoordinator::new(clock, Vec::new(), 5).unwrap();
    assert_eq!(
        coordinator.add_pending(file(), report_only()),
        Err(CoordinatorError::MissingProvider)
    );
    assert!(coordinator.snapshots().is_empty());
    let id = coordinator
        .add_pending(file(), ProviderSelection::default())
        .unwrap();
    assert_eq!(id.get(), 1);
    coordinator.set_metadata(id, metadata(123)).unwrap();
    let before = coordinator.snapshots();
    assert_eq!(
        coordinator.reupload(id),
        Err(CoordinatorError::MissingProvider)
    );
    assert_eq!(coordinator.snapshots(), before);
}

#[test]
fn failed_retry_enqueue_never_leaves_an_unqueued_active_attempt() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(report_only(), 123);
    fixture.complete(Provider::DpsReport, UploadOutcome::Failed);
    fixture.states[0].lock().unwrap().capacity = 1;
    assert_eq!(
        fixture.coordinator.retry_failed(id, Provider::DpsReport),
        Err(CoordinatorError::Dispatch(ProviderError::QueueFull))
    );
    assert_eq!(
        fixture.state(id, Provider::DpsReport),
        ProviderState::Failed
    );
    assert_eq!(fixture.coordinator.history()[0].providers[0].attempts, 2);
}

#[test]
fn retry_budget_still_trims_jobs_settled_by_dispatch_rejection() {
    let mut fixture = Fixture::new(2);
    let first = fixture.start(report_only(), 123);
    fixture.complete(
        Provider::DpsReport,
        UploadOutcome::RetryAfter(Duration::from_secs(1)),
    );
    fixture.start(report_only(), 123);
    fixture.coordinator.set_history_limit(1).unwrap();
    fixture.states[0].lock().unwrap().capacity = 2;
    fixture.clock.advance(1);
    assert_eq!(fixture.coordinator.dispatch_due_retries(1), 1);
    let snapshots = fixture.coordinator.snapshots();
    assert_eq!(snapshots.len(), 1);
    assert_ne!(snapshots[0].id, first);
}

#[test]
fn fanout_rejection_preserves_the_report_and_other_destinations() {
    let mut fixture = Fixture::new(5);
    let id = fixture.start(all(), 123);
    fixture.states[Provider::Wingman as usize]
        .lock()
        .unwrap()
        .capacity = 0;
    fixture.complete(Provider::DpsReport, report("retained"));
    assert_eq!(
        fixture.state(id, Provider::DpsReport),
        ProviderState::Succeeded
    );
    assert_eq!(fixture.state(id, Provider::Wingman), ProviderState::Failed);
    assert_eq!(fixture.state(id, Provider::DonBot), ProviderState::Active);
    assert_eq!(fixture.state(id, Provider::Twitch), ProviderState::Active);
    fixture.states[Provider::Wingman as usize]
        .lock()
        .unwrap()
        .capacity = 1;
    fixture
        .coordinator
        .retry_failed(id, Provider::Wingman)
        .unwrap();
    assert_eq!(
        fixture
            .last(Provider::Wingman)
            .dps_report_result
            .unwrap()
            .permalink,
        "https://dps.report/retained"
    );
}

#[test]
fn pending_metadata_is_retained_even_when_no_provider_is_enabled() {
    let mut fixture = Fixture::new(1);
    let first = fixture
        .coordinator
        .add_pending(file(), ProviderSelection::default())
        .unwrap();
    assert_eq!(
        fixture.coordinator.add_pending(file(), report_only()),
        Err(CoordinatorError::CapacityReached)
    );
    fixture.coordinator.fail_metadata(first, false).unwrap();
    assert_eq!(
        fixture.coordinator.set_metadata(first, metadata(123)),
        Err(CoordinatorError::MetadataNotPending)
    );
    assert!(
        fixture
            .coordinator
            .add_pending(file(), report_only())
            .is_ok()
    );
}
