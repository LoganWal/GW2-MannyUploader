use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use manny_uploader::application::*;
use manny_uploader::domain::LogFileIdentity;
use manny_uploader::filesystem::NotifyingLogSource;

struct State {
    batch: Result<CandidateBatch, CandidateError>,
    change: Result<DirectoryChange, CandidateError>,
    configure_fails: bool,
    monitor_panics: bool,
    scans: usize,
    polls: usize,
}
struct Source(Arc<Mutex<State>>);
struct Monitor(Arc<Mutex<State>>);
impl LogCandidateSource for Source {
    fn poll(&mut self, _: &StopToken) -> Result<CandidateBatch, CandidateError> {
        let mut state = self.0.lock().unwrap();
        state.scans += 1;
        state.batch.clone()
    }
    fn reconfigure(&mut self, options: CandidateSourceOptions) -> Result<(), CandidateError> {
        if options.root.as_os_str().is_empty() {
            Err(CandidateError::InvalidConfiguration)
        } else {
            Ok(())
        }
    }
}
impl DirectoryChangeMonitor for Monitor {
    fn reconfigure(&mut self, _: &std::path::Path, _: bool) -> Result<(), CandidateError> {
        if self.0.lock().unwrap().configure_fails {
            Err(CandidateError::ScanFailed)
        } else {
            Ok(())
        }
    }
    fn poll(&mut self, _: &StopToken) -> Result<DirectoryChange, CandidateError> {
        let mut state = self.0.lock().unwrap();
        state.polls += 1;
        if state.monitor_panics {
            drop(state);
            panic!("synthetic monitor panic");
        }
        state.change
    }
}
fn options() -> CandidateSourceOptions {
    CandidateSourceOptions {
        root: "logs".into(),
        recursive: true,
        max_candidates: 8,
        minimum_last_write_time: None,
    }
}
fn fixture() -> (NotifyingLogSource, Arc<Mutex<State>>, StopToken) {
    let state = Arc::new(Mutex::new(State {
        batch: Ok(CandidateBatch {
            root_available: true,
            scan_complete: true,
            observations: vec![LogFileIdentity {
                canonical_path: "logs/a.zevtc".into(),
                size: 1,
                last_write_time: SystemTime::UNIX_EPOCH,
            }],
            removed_paths: vec!["logs/old.zevtc".into()],
            issues: vec![CandidateIssue::EntryUnavailable],
        }),
        change: Ok(DirectoryChange::Unchanged),
        configure_fails: false,
        monitor_panics: false,
        scans: 0,
        polls: 0,
    }));
    let source = NotifyingLogSource::new(
        Box::new(Source(state.clone())),
        Some(Box::new(Monitor(state.clone()))),
        options(),
    )
    .unwrap();
    (source, state, CancellationSource::default().token())
}

#[test]
fn cache_repeats_only_observations_and_changed_notifications_rescan() {
    let (mut source, state, stop) = fixture();
    let initial = source.poll(&stop).unwrap();
    let cached = source.poll(&stop).unwrap();
    assert_eq!(cached.observations, initial.observations);
    assert!(cached.removed_paths.is_empty() && cached.issues.is_empty());
    assert_eq!(state.lock().unwrap().scans, 1);
    state.lock().unwrap().change = Ok(DirectoryChange::Changed);
    assert_eq!(source.poll(&stop).unwrap(), initial);
    assert_eq!(state.lock().unwrap().scans, 2);
}

#[test]
fn failed_or_incomplete_authoritative_scan_cannot_revive_stale_cache() {
    let (mut source, state, stop) = fixture();
    source.poll(&stop).unwrap();
    state.lock().unwrap().change = Ok(DirectoryChange::Changed);
    state.lock().unwrap().batch = Err(CandidateError::ResourceLimit);
    assert_eq!(source.poll(&stop), Err(CandidateError::ResourceLimit));
    state.lock().unwrap().change = Ok(DirectoryChange::Unchanged);
    assert_eq!(source.poll(&stop), Err(CandidateError::ResourceLimit));
    for (root, complete) in [(true, false), (false, true)] {
        state.lock().unwrap().batch = Ok(CandidateBatch {
            root_available: root,
            scan_complete: complete,
            ..Default::default()
        });
        source.poll(&stop).unwrap();
        let scans = state.lock().unwrap().scans;
        source.poll(&stop).unwrap();
        assert_eq!(state.lock().unwrap().scans, scans + 1);
    }
}

#[test]
fn three_consecutive_failures_fall_back_until_successful_reconfiguration() {
    let (mut source, state, stop) = fixture();
    state.lock().unwrap().change = Err(CandidateError::ScanFailed);
    for index in 1..=3 {
        let batch = source.poll(&stop).unwrap();
        assert!(batch.issues.contains(&if index == 3 {
            CandidateIssue::NotificationsDisabled
        } else {
            CandidateIssue::NotificationFailed
        }));
        assert_eq!(source.consecutive_failures(), index);
    }
    assert!(!source.using_notifications());
    let polls = state.lock().unwrap().polls;
    source.poll(&stop).unwrap();
    assert_eq!(state.lock().unwrap().polls, polls);
    let mut invalid = options();
    invalid.root.clear();
    assert_eq!(
        source.reconfigure(invalid),
        Err(CandidateError::InvalidConfiguration)
    );
    assert!(!source.using_notifications());
    source.reconfigure(options()).unwrap();
    assert!(source.using_notifications());
    assert_eq!(source.consecutive_failures(), 0);
    state.lock().unwrap().change = Ok(DirectoryChange::Unchanged);
    let scans = state.lock().unwrap().scans;
    source.poll(&stop).unwrap();
    assert_eq!(state.lock().unwrap().scans, scans + 1);
}

#[test]
fn success_resets_failures_and_monitor_panics_use_polling() {
    let (mut source, state, stop) = fixture();
    state.lock().unwrap().change = Err(CandidateError::ScanFailed);
    source.poll(&stop).unwrap();
    state.lock().unwrap().change = Ok(DirectoryChange::Unavailable);
    source.poll(&stop).unwrap();
    assert_eq!(source.consecutive_failures(), 0);
    state.lock().unwrap().monitor_panics = true;
    assert!(
        source
            .poll(&stop)
            .unwrap()
            .issues
            .contains(&CandidateIssue::NotificationFailed)
    );
    assert_eq!(source.consecutive_failures(), 1);
}

#[test]
fn setup_failure_reports_once_and_cancellation_never_queries_or_scans() {
    let (mut source, state, stop) = fixture();
    state.lock().unwrap().configure_fails = true;
    source.reconfigure(options()).unwrap();
    assert!(!source.using_notifications());
    assert!(
        source
            .poll(&stop)
            .unwrap()
            .issues
            .contains(&CandidateIssue::NotificationsDisabled)
    );
    assert!(
        !source
            .poll(&stop)
            .unwrap()
            .issues
            .contains(&CandidateIssue::NotificationsDisabled)
    );
    let cancelled = CancellationSource::default();
    cancelled.request_stop();
    let scans = state.lock().unwrap().scans;
    assert_eq!(
        source.poll(&cancelled.token()),
        Err(CandidateError::Cancelled)
    );
    assert_eq!(state.lock().unwrap().scans, scans);
    state.lock().unwrap().configure_fails = false;
    source.reconfigure(options()).unwrap();
    state.lock().unwrap().change = Err(CandidateError::Cancelled);
    assert_eq!(source.poll(&stop), Err(CandidateError::Cancelled));
    assert_eq!(state.lock().unwrap().scans, scans);
}
