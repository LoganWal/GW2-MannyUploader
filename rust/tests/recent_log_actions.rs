use manny_uploader::{application::*, domain::*, ui::*};
use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Instant, SystemTime},
};
type Error = RecentLogActionError;
struct TestClock;
impl Clock for TestClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}
#[derive(Default)]
struct Requests {
    values: Vec<UploadRequest>,
    reject: Option<Provider>,
}
struct Worker(Provider, Arc<Mutex<Requests>>);
impl UploadProvider for Worker {
    fn provider(&self) -> Provider {
        self.0
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        let mut requests = self.1.lock().unwrap();
        if requests.reject == Some(self.0) {
            return Err(ProviderError::QueueFull);
        }
        requests.values.push(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        Ok(None)
    }
    fn cancel_pending(&mut self) {}
}
#[derive(Debug, PartialEq, Eq)]
enum Target {
    Url(String),
    Directory(PathBuf),
}
type LaunchHook = dyn FnMut(&Target) -> Result<(), ExternalActionError> + Send;
#[derive(Default)]
struct Launcher {
    targets: Vec<Target>,
    hook: Option<Box<LaunchHook>>,
}
impl Launcher {
    fn launch(&mut self, target: Target) -> Result<(), ExternalActionError> {
        self.targets.push(target);
        if let Some(hook) = &mut self.hook {
            hook(self.targets.last().unwrap())
        } else {
            Ok(())
        }
    }
}
impl ExternalActionLauncher for Launcher {
    fn open_url(&mut self, url: &str) -> Result<(), ExternalActionError> {
        self.launch(Target::Url(url.into()))
    }
    fn open_directory(&mut self, directory: &Path) -> Result<(), ExternalActionError> {
        self.launch(Target::Directory(directory.into()))
    }
}
struct Harness {
    uploads: UploadCoordinator<TestClock>,
    requests: Arc<Mutex<Requests>>,
    launcher: Launcher,
    actions: Arc<RecentLogActionsController>,
}
impl Harness {
    fn new() -> Self {
        let requests = Arc::new(Mutex::new(Requests::default()));
        let uploads = UploadCoordinator::new(
            TestClock,
            Provider::ALL
                .into_iter()
                .map(|provider| {
                    Box::new(Worker(provider, requests.clone())) as Box<dyn UploadProvider>
                })
                .collect(),
            8,
        )
        .unwrap();
        Self {
            uploads,
            requests,
            launcher: Launcher::default(),
            actions: Arc::new(
                RecentLogActionsController::new(RecentLogActionsOptions::default()).unwrap(),
            ),
        }
    }
    fn pending(&mut self, path: &str) -> UploadJobId {
        self.uploads
            .add_pending(
                LogFileIdentity {
                    canonical_path: path.into(),
                    size: 10,
                    last_write_time: SystemTime::UNIX_EPOCH,
                },
                ProviderSelection {
                    dps_report: true,
                    wingman: true,
                    donbot: true,
                    twitch: true,
                },
            )
            .unwrap()
    }
    fn complete(&mut self, twitch: UploadOutcome) -> UploadJobId {
        let id = self.pending("/synthetic/\u{65e5}\u{672c}/encounter.zevtc");
        self.uploads
            .set_metadata(
                id,
                EncounterMetadata {
                    boss_id: 42,
                    pov_account: "Fixture.1234".into(),
                    remaining_health_basis_points: None,
                },
            )
            .unwrap();
        self.result(
            id,
            Provider::DpsReport,
            UploadOutcome::Succeeded(UploadSuccess::DpsReport(report())),
        );
        self.result(
            id,
            Provider::Wingman,
            UploadOutcome::Succeeded(UploadSuccess::Wingman(Some(WingmanUploadReceipt {
                permalink: "https://gw2wingman.nevermindcreations.de/log/fixture".into(),
            }))),
        );
        self.result(
            id,
            Provider::DonBot,
            UploadOutcome::Succeeded(UploadSuccess::DonBot(Some(DonBotUploadReceipt {
                upload_id: Some(17),
                fight_log_id: Some(123),
                ..Default::default()
            }))),
        );
        self.result(id, Provider::Twitch, twitch);
        id
    }
    fn result(&mut self, id: UploadJobId, provider: Provider, outcome: UploadOutcome) {
        let attempt =
            self.uploads.snapshot(id).unwrap().record.providers[provider as usize].attempts;
        self.uploads
            .apply_result(UploadResult {
                job_id: id,
                provider,
                attempt,
                outcome,
            })
            .unwrap();
    }
    fn tick(&mut self) -> RecentLogActionsTickReport {
        self.actions
            .tick(&mut self.uploads, &mut self.launcher)
            .unwrap()
    }
    fn act(&mut self, action: RecentLogAction) -> RecentLogActionsTickReport {
        self.actions.submit(action).unwrap();
        self.tick()
    }
}
fn report() -> DpsReportResult {
    DpsReportResult {
        permalink: "https://dps.report/fixture".into(),
        encounter_name: "Boss".into(),
        boss_id: 42,
        mode: "CM".into(),
        success: true,
    }
}
fn sent() -> UploadOutcome {
    UploadOutcome::Succeeded(UploadSuccess::Twitch(TwitchDeliveryReceipt {
        status: TwitchDeliveryStatus::Sent,
        message_id: Some("synthetic-message".into()),
    }))
}
fn id(value: u64) -> UploadJobId {
    UploadJobId::new(value).unwrap()
}

#[test]
fn submission_has_no_external_effect_and_tick_resolves_exact_current_targets() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    for target in [
        ReportTarget::DpsReport,
        ReportTarget::Wingman,
        ReportTarget::DonBot,
    ] {
        h.actions
            .submit(RecentLogAction::OpenReport {
                job_id: job,
                target,
            })
            .unwrap();
    }
    h.actions
        .submit(RecentLogAction::OpenDirectory(job))
        .unwrap();
    assert!(h.launcher.targets.is_empty());
    assert_eq!(
        h.tick(),
        RecentLogActionsTickReport {
            commands_processed: 4,
            action_failures: 0
        }
    );
    assert_eq!(
        h.launcher.targets,
        [
            Target::Url("https://dps.report/fixture".into()),
            Target::Url("https://gw2wingman.nevermindcreations.de/log/fixture".into()),
            Target::Url("https://donbot.walmslo.com/logs/123".into()),
            Target::Directory("/synthetic/\u{65e5}\u{672c}".into())
        ]
    );
}

#[test]
fn report_targets_reject_hostile_urls_and_gate_wvw_origin_by_retained_boss() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    let base = h.uploads.snapshot(job).unwrap();
    for url in ["https://dps.report/fixture", "https://b.dps.report/fixture"] {
        let mut value = base.clone();
        value.record.dps_report_result.as_mut().unwrap().permalink = url.into();
        assert_eq!(report_url(&value, ReportTarget::DpsReport).unwrap(), url);
    }
    for url in [
        "https://dps.report/",
        "https://dps.report.evil.invalid/x",
        "https://dps.report@evil.invalid/x",
        "http://dps.report/x",
        "https://dps.report/a\\b",
        "https://dps.report/a\"b",
        "https://dps.report/a\nb",
        "https://dps.report/\u{65e5}",
        "https://wvw.report/fixture",
    ] {
        let mut value = base.clone();
        value.record.dps_report_result.as_mut().unwrap().permalink = url.into();
        assert_eq!(
            report_url(&value, ReportTarget::DpsReport),
            Err(Error::UnsafeReportUrl),
            "{url:?}"
        );
    }
    let mut value = base.clone();
    value.record.dps_report_result.as_mut().unwrap().permalink =
        format!("https://dps.report/{}", "x".repeat(2048));
    assert_eq!(
        report_url(&value, ReportTarget::DpsReport),
        Err(Error::UnsafeReportUrl)
    );
    value.record.dps_report_result.as_mut().unwrap().permalink =
        "https://wvw.report/fixture".into();
    value.record.dps_report_result.as_mut().unwrap().boss_id = 1;
    assert!(report_url(&value, ReportTarget::DpsReport).is_ok());
    for url in [
        "https://gw2wingman.nevermindcreations.de/log/",
        "https://gw2wingman.nevermindcreations.de.evil/log/x",
        "https://gw2wingman.nevermindcreations.de/log/\"x",
    ] {
        let mut value = base.clone();
        value
            .record
            .wingman_upload_receipt
            .as_mut()
            .unwrap()
            .permalink = url.into();
        assert_eq!(
            report_url(&value, ReportTarget::Wingman),
            Err(Error::UnsafeReportUrl)
        );
    }
}

#[test]
fn missing_receipts_and_invalid_donbot_ids_have_no_targets() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    let mut value = h.uploads.snapshot(job).unwrap();
    value.record.dps_report_result = None;
    value.record.wingman_upload_receipt = None;
    for target in [ReportTarget::DpsReport, ReportTarget::Wingman] {
        assert_eq!(report_url(&value, target), Err(Error::ReportUnavailable));
    }
    for fight in [None, Some(0), Some(i64::MAX as u64 + 1)] {
        value
            .record
            .donbot_upload_receipt
            .as_mut()
            .unwrap()
            .fight_log_id = fight;
        assert_eq!(
            report_url(&value, ReportTarget::DonBot),
            Err(Error::ReportUnavailable)
        );
    }
    value
        .record
        .donbot_upload_receipt
        .as_mut()
        .unwrap()
        .fight_log_id = Some(i64::MAX as u64);
    assert_eq!(
        report_url(&value, ReportTarget::DonBot).unwrap(),
        "https://donbot.walmslo.com/logs/9223372036854775807"
    );
}

#[test]
fn queued_actions_re_resolve_jobs_after_history_eviction() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    h.actions
        .submit(RecentLogAction::OpenDirectory(job))
        .unwrap();
    h.uploads.set_history_limit(1).unwrap();
    h.complete(sent());
    assert_eq!(h.tick().action_failures, 1);
    assert_eq!(h.actions.snapshot().last_error, Some(Error::UnknownJob));
    assert!(h.launcher.targets.is_empty());
}

#[test]
fn directory_without_a_parent_and_unavailable_report_fail_without_launching() {
    let mut h = Harness::new();
    let job = h.pending("filename.zevtc");
    assert_eq!(
        h.act(RecentLogAction::OpenDirectory(job)).action_failures,
        1
    );
    assert_eq!(
        h.actions.snapshot().last_error,
        Some(Error::InvalidDirectory)
    );
    assert_eq!(
        h.act(RecentLogAction::OpenReport {
            job_id: job,
            target: ReportTarget::DpsReport
        })
        .action_failures,
        1
    );
    assert_eq!(
        h.actions.snapshot().last_error,
        Some(Error::ReportUnavailable)
    );
    assert!(h.launcher.targets.is_empty());
}

#[test]
fn queue_bounds_and_per_tick_limits_hold_with_concurrent_submitters() {
    let actions =
        Arc::new(RecentLogActionsController::new(RecentLogActionsOptions::default()).unwrap());
    let mut threads = Vec::new();
    for _ in 0..4 {
        let actions = actions.clone();
        threads.push(std::thread::spawn(move || {
            for _ in 0..8 {
                actions.submit(RecentLogAction::DismissError).unwrap();
            }
        }));
    }
    for thread in threads {
        thread.join().unwrap();
    }
    assert_eq!(actions.snapshot().pending_commands, 32);
    assert_eq!(
        actions.submit(RecentLogAction::DismissError),
        Err(Error::QueueFull)
    );
    let mut h = Harness::new();
    assert_eq!(
        actions
            .tick(&mut h.uploads, &mut h.launcher)
            .unwrap()
            .commands_processed,
        8
    );
    assert_eq!(actions.snapshot().pending_commands, 24);
    for _ in 0..3 {
        actions.tick(&mut h.uploads, &mut h.launcher).unwrap();
    }
    assert_eq!(actions.snapshot().pending_commands, 0);
    assert!(actions.snapshot().last_error.is_none());
}

#[test]
fn popping_a_command_publishes_a_new_revision_before_external_work() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    h.actions
        .submit(RecentLogAction::OpenDirectory(job))
        .unwrap();
    let queued = h.actions.snapshot();
    let actions = h.actions.clone();
    h.launcher.hook = Some(Box::new(move |_| {
        let current = actions.snapshot();
        assert_eq!(current.pending_commands, 0);
        assert!(current.revision > queued.revision);
        Ok(())
    }));
    assert_eq!(h.tick().action_failures, 0);
}

#[test]
fn queue_lock_is_released_for_launchers_and_new_commands_wait_until_the_next_tick() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    let actions = h.actions.clone();
    h.launcher.hook = Some(Box::new(move |_| {
        actions.submit(RecentLogAction::DismissError).unwrap();
        let mut nested = Harness::new();
        assert_eq!(
            actions.tick(&mut nested.uploads, &mut nested.launcher),
            Err(Error::Busy)
        );
        Ok(())
    }));
    assert_eq!(
        h.act(RecentLogAction::OpenDirectory(job))
            .commands_processed,
        1
    );
    assert_eq!(h.actions.snapshot().pending_commands, 1);
    assert_eq!(h.tick().commands_processed, 1);
}

#[test]
fn launcher_failure_and_panic_are_typed_and_do_not_poison_the_controller() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    for panic in [false, true] {
        h.launcher.hook = Some(Box::new(move |_| {
            if panic {
                panic!("synthetic-private-panic")
            }
            Err(ExternalActionError::LaunchFailed)
        }));
        assert_eq!(
            h.act(RecentLogAction::OpenDirectory(job)).action_failures,
            1
        );
        assert_eq!(
            h.actions.snapshot().last_error,
            Some(if panic {
                Error::Internal
            } else {
                Error::LaunchFailed(ExternalActionError::LaunchFailed)
            })
        );
        let debug = format!("{:?}", h.actions.snapshot());
        assert!(!debug.contains("synthetic-private-panic"));
        assert!(!debug.contains("encounter.zevtc"));
        h.act(RecentLogAction::DismissError);
        assert!(h.actions.snapshot().last_error.is_none());
    }
}

#[test]
fn shutdown_during_a_launch_clears_remaining_commands_and_blocks_late_publication() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    let actions = h.actions.clone();
    h.launcher.hook = Some(Box::new(move |_| {
        actions.shutdown();
        Err(ExternalActionError::LaunchFailed)
    }));
    for _ in 0..2 {
        h.actions
            .submit(RecentLogAction::OpenDirectory(job))
            .unwrap();
    }
    assert_eq!(
        h.tick(),
        RecentLogActionsTickReport {
            commands_processed: 1,
            action_failures: 1
        }
    );
    assert_eq!(h.launcher.targets.len(), 1);
    let snapshot = h.actions.snapshot();
    assert!(snapshot.shutting_down);
    assert!(!snapshot.accepting_commands);
    assert_eq!(snapshot.pending_commands, 0);
    assert!(snapshot.last_error.is_none());
    h.actions.shutdown();
    assert_eq!(h.actions.snapshot(), snapshot);
    assert_eq!(
        h.actions.submit(RecentLogAction::DismissError),
        Err(Error::ShuttingDown)
    );
    assert_eq!(
        h.actions.tick(&mut h.uploads, &mut h.launcher),
        Err(Error::ShuttingDown)
    );
}

#[test]
fn failed_twitch_retry_is_explicit_and_dispatch_failure_returns_to_failed() {
    let mut h = Harness::new();
    let job = h.complete(UploadOutcome::Ambiguous);
    h.requests.lock().unwrap().values.clear();
    assert_eq!(
        h.act(RecentLogAction::Retry {
            job_id: job,
            provider: Provider::Twitch
        })
        .action_failures,
        0
    );
    let requests = h.requests.lock().unwrap();
    assert_eq!(requests.values.len(), 1);
    assert_eq!(
        (requests.values[0].intent, requests.values[0].attempt),
        (UploadIntent::ManualRetry, 2)
    );
    drop(requests);
    h.result(job, Provider::Twitch, UploadOutcome::Ambiguous);
    h.requests.lock().unwrap().reject = Some(Provider::Twitch);
    assert_eq!(
        h.act(RecentLogAction::Retry {
            job_id: job,
            provider: Provider::Twitch
        })
        .action_failures,
        1
    );
    assert_eq!(
        h.uploads.snapshot(job).unwrap().record.providers[Provider::Twitch as usize].state,
        ProviderState::Failed
    );
    assert_eq!(
        h.actions.snapshot().last_error,
        Some(Error::RetryFailed(CoordinatorError::Dispatch(
            ProviderError::QueueFull
        )))
    );
}

#[test]
fn replay_actions_preserve_their_distinct_provider_intents() {
    let mut h = Harness::new();
    let job = h.complete(sent());
    h.requests.lock().unwrap().values.clear();
    assert_eq!(
        h.act(RecentLogAction::Retry {
            job_id: job,
            provider: Provider::Twitch
        })
        .action_failures,
        1
    );
    assert_eq!(h.act(RecentLogAction::Reupload(job)).action_failures, 0);
    let during = h.uploads.snapshot(job).unwrap();
    assert!(during.record.dps_report_result.is_none());
    assert!(during.record.wingman_upload_receipt.is_none());
    assert!(during.record.donbot_upload_receipt.is_none());
    assert!(during.record.twitch_delivery_receipt.is_some());
    h.result(
        job,
        Provider::DpsReport,
        UploadOutcome::Succeeded(UploadSuccess::DpsReport(report())),
    );
    assert_eq!(h.act(RecentLogAction::Rechat(job)).action_failures, 0);
    let requests = h.requests.lock().unwrap();
    assert_eq!(requests.values.len(), 4);
    for request in &requests.values[..3] {
        assert_eq!(request.intent, UploadIntent::Reupload);
        assert_ne!(request.provider, Provider::Twitch);
    }
    assert_eq!(requests.values[3].intent, UploadIntent::Rechat);
    assert_eq!(requests.values[3].provider, Provider::Twitch);
    assert!(
        h.uploads
            .snapshot(job)
            .unwrap()
            .record
            .twitch_delivery_receipt
            .is_none()
    );
}

#[test]
fn metadata_and_busy_state_prevent_unsafe_replays() {
    let mut h = Harness::new();
    let job = h.pending("synthetic/log.zevtc");
    for command in [
        RecentLogAction::Reupload(job),
        RecentLogAction::Rechat(job),
        RecentLogAction::Retry {
            job_id: job,
            provider: Provider::DpsReport,
        },
    ] {
        assert_eq!(h.act(command).action_failures, 1);
    }
    h.uploads
        .set_metadata(
            job,
            EncounterMetadata {
                boss_id: 42,
                pov_account: String::new(),
                remaining_health_basis_points: None,
            },
        )
        .unwrap();
    assert_eq!(h.act(RecentLogAction::Reupload(job)).action_failures, 1);
    assert_eq!(h.requests.lock().unwrap().values.len(), 1);
}

#[test]
fn invalid_queue_configuration_is_rejected_before_use() {
    for options in [
        RecentLogActionsOptions {
            command_capacity: 0,
            max_commands_per_tick: 1,
        },
        RecentLogActionsOptions {
            command_capacity: 1,
            max_commands_per_tick: 0,
        },
    ] {
        assert!(matches!(
            RecentLogActionsController::new(options),
            Err(Error::InvalidConfiguration)
        ));
    }
}

fn selection_identity(guild: &str) -> Option<DonBotSelectionIdentity> {
    Some(DonBotSelectionIdentity {
        api_base_url: "https://donbot.invalid".into(),
        account_name: "Fixture.1234".into(),
        guild_id: guild.into(),
    })
}
fn candidate(value: u64, eligible: bool) -> DonBotAggregateCandidate {
    DonBotAggregateCandidate {
        job_id: id(value),
        eligible,
    }
}

#[test]
fn selection_defaults_only_newly_eligible_rows_and_preserves_deselection() {
    let mut selection = DonBotAggregateSelection::default();
    let retained = [id(1), id(2), id(3)];
    selection.reconcile(
        selection_identity("123"),
        &retained,
        &[candidate(1, true), candidate(2, false)],
    );
    assert!(selection.selected(id(1)));
    assert!(!selection.selected(id(2)));
    selection.set_selected(id(1), false);
    selection.reconcile(
        selection_identity("123"),
        &retained,
        &[candidate(1, true), candidate(2, true), candidate(3, true)],
    );
    assert!(!selection.selected(id(1)));
    assert!(selection.selected(id(2)));
    assert!(selection.selected(id(3)));
}

#[test]
fn temporary_ineligibility_and_hidden_rows_preserve_selection_intent() {
    let mut selection = DonBotAggregateSelection::default();
    let retained = [id(1), id(2)];
    selection.reconcile(
        selection_identity("123"),
        &retained,
        &[candidate(1, true), candidate(2, true)],
    );
    selection.set_selected(id(1), false);
    selection.reconcile(
        selection_identity("123"),
        &retained,
        &[candidate(1, false), candidate(2, false)],
    );
    assert!(selection.selected(id(2)));
    selection.reconcile(selection_identity("123"), &retained, &[]);
    let visible = [candidate(2, true), candidate(1, true), candidate(2, true)];
    selection.reconcile(selection_identity("123"), &retained, &visible);
    assert_eq!(selection.selected_visible(&visible), [id(2)]);
}

#[test]
fn identity_changes_reset_selection_and_eviction_prunes_all_row_state() {
    let mut selection = DonBotAggregateSelection::default();
    let visible = [candidate(1, true), candidate(2, true), candidate(3, true)];
    selection.reconcile(selection_identity("123"), &[id(1), id(2), id(3)], &visible);
    selection.set_selected(id(1), false);
    selection.reconcile(selection_identity("123"), &[id(1), id(2)], &visible);
    assert!(!selection.selected(id(3)));
    selection.set_selected(id(3), true);
    assert!(!selection.selected(id(3)));
    selection.reconcile(selection_identity("456"), &[id(1), id(2)], &visible);
    assert_eq!(selection.selected_visible(&visible), [id(1), id(2)]);
    let mut changed = selection_identity("456");
    changed.as_mut().unwrap().account_name = "Other.5678".into();
    selection.set_selected(id(1), false);
    selection.reconcile(changed, &[id(1), id(2)], &visible);
    assert!(selection.selected(id(1)));
}
