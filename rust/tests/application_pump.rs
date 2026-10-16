use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::{Instant, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::*;

struct TestClock;
impl Clock for TestClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}

fn file(name: &str) -> LogFileIdentity {
    LogFileIdentity {
        canonical_path: name.into(),
        size: 1,
        last_write_time: SystemTime::UNIX_EPOCH,
    }
}

#[derive(Default)]
struct SourceState {
    batch: CandidateBatch,
    error: Option<CandidateError>,
    panic: bool,
    polls: usize,
}
struct Source(Arc<Mutex<SourceState>>);
impl LogCandidateSource for Source {
    fn poll(&mut self, _: &StopToken) -> Result<CandidateBatch, CandidateError> {
        let mut state = self.0.lock().unwrap();
        state.polls += 1;
        if state.panic {
            drop(state);
            panic!("synthetic source panic");
        }
        if let Some(error) = state.error {
            Err(error)
        } else {
            Ok(state.batch.clone())
        }
    }
    fn reconfigure(&mut self, options: CandidateSourceOptions) -> Result<(), CandidateError> {
        if options.root.as_os_str().is_empty() {
            Err(CandidateError::InvalidConfiguration)
        } else {
            Ok(())
        }
    }
}

#[derive(Default)]
struct ParserState {
    capacity: usize,
    requests: Vec<MetadataRequest>,
    results: VecDeque<MetadataResult>,
    stopped: usize,
    reject: bool,
}
struct Parser(Arc<Mutex<ParserState>>);
impl LogMetadataParser for Parser {
    fn available_capacity(&self) -> usize {
        self.0.lock().unwrap().capacity
    }
    fn enqueue(&mut self, request: MetadataRequest) -> Result<(), ParserError> {
        let mut state = self.0.lock().unwrap();
        if state.reject {
            return Err(ParserError::Unavailable);
        }
        state.capacity -= 1;
        state.requests.push(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Result<Option<MetadataResult>, ParserError> {
        let mut state = self.0.lock().unwrap();
        let result = state.results.pop_front();
        if result.is_some() {
            state.capacity += 1;
        }
        Ok(result)
    }
    fn cancel_pending(&mut self) {
        self.0.lock().unwrap().stopped += 1;
    }
}

struct Fixture {
    pump: ApplicationPump<TestClock>,
    parser: Arc<Mutex<ParserState>>,
    source: Arc<Mutex<SourceState>>,
    stop: CancellationSource,
}
impl Fixture {
    fn new(capacity: usize, history: usize) -> Self {
        let parser = Arc::new(Mutex::new(ParserState {
            capacity,
            ..Default::default()
        }));
        let source = Arc::new(Mutex::new(SourceState {
            batch: CandidateBatch {
                root_available: true,
                scan_complete: true,
                observations: vec![file("a.zevtc"), file("b.zevtc")],
                ..Default::default()
            },
            ..Default::default()
        }));
        let ingestion = LogIngestionCoordinator::new(
            UploadCoordinator::new(TestClock, vec![], history).unwrap(),
            Box::new(Parser(parser.clone())),
        );
        let options = PumpOptions {
            max_metadata_results: 1,
            ..Default::default()
        };
        Self {
            pump: ApplicationPump::new(Box::new(Source(source.clone())), ingestion, options)
                .unwrap(),
            parser,
            source,
            stop: CancellationSource::default(),
        }
    }
    fn tick(&mut self) -> Result<TickReport, PumpError> {
        self.pump
            .tick(ProviderSelection::default(), &self.stop.token())
    }
    fn complete(&self, index: usize) {
        let mut parser = self.parser.lock().unwrap();
        let job_id = parser.requests[index].job_id;
        parser.results.push_back(MetadataResult {
            job_id,
            metadata: Ok(EncounterMetadata {
                boss_id: 1,
                pov_account: ":Player.1234".into(),
                remaining_health_basis_points: None,
            }),
        });
    }
    fn reconfigure(&mut self, root: &str) -> Result<(), PumpError> {
        self.pump.reconfigure_source(CandidateSourceOptions {
            root: root.into(),
            recursive: true,
            max_candidates: 8,
            minimum_last_write_time: None,
        })
    }
}

#[test]
fn pump_defers_at_capacity_and_drains_before_admitting_more_files() {
    let mut f = Fixture::new(1, 4);
    assert_eq!(f.tick().unwrap().submitted, 0);
    assert_eq!(f.tick().unwrap().submitted, 1);
    for _ in 0..4 {
        assert_eq!(f.tick().unwrap().submitted, 0);
    }
    assert_eq!(f.parser.lock().unwrap().requests.len(), 1);
    f.complete(0);
    let report = f.tick().unwrap();
    assert_eq!((report.metadata.consumed, report.submitted), (1, 1));
    f.complete(1);
    assert_eq!(f.tick().unwrap().submitted, 0);
    assert_eq!(f.pump.ingestion().uploads().snapshots().len(), 2);
}

#[test]
fn failed_admission_releases_dedupe_and_retries_when_history_has_room() {
    let mut f = Fixture::new(4, 1);
    f.tick().unwrap();
    assert_eq!(
        f.tick(),
        Err(PumpError::Ingestion(IngestionError::Coordinator(
            CoordinatorError::CapacityReached
        )))
    );
    assert_eq!(f.parser.lock().unwrap().requests.len(), 1);
    f.complete(0);
    assert_eq!(f.tick().unwrap().submitted, 0);
    assert_eq!(f.tick().unwrap().submitted, 1);
    assert_eq!(
        f.parser.lock().unwrap().requests[1]
            .file
            .canonical_path
            .to_str(),
        Some("b.zevtc")
    );
}

#[test]
fn unexpected_parser_rejection_remains_accepted_and_is_not_replayed() {
    let mut f = Fixture::new(4, 4);
    f.parser.lock().unwrap().reject = true;
    f.tick().unwrap();
    assert_eq!(f.tick().unwrap().submitted, 2);
    f.parser.lock().unwrap().reject = false;
    for _ in 0..4 {
        assert_eq!(f.tick().unwrap().submitted, 0);
    }
    assert_eq!(f.pump.ingestion().uploads().snapshots().len(), 2);
    assert!(f.parser.lock().unwrap().requests.is_empty());
}

#[test]
fn reconfiguration_clears_only_pending_samples_and_preserves_seeded_history() {
    let mut f = Fixture::new(4, 4);
    f.pump.seed_processed_logs(&[file("a.zevtc")]).unwrap();
    f.tick().unwrap();
    assert_eq!(
        f.reconfigure(""),
        Err(PumpError::Source(CandidateError::InvalidConfiguration))
    );
    assert_eq!(
        f.pump.set_required_matches(1),
        Err(PumpError::Discovery(DiscoveryError::InvalidConfiguration))
    );
    f.reconfigure("replacement").unwrap();
    assert_eq!(f.tick().unwrap().submitted, 0);
    assert_eq!(f.tick().unwrap().submitted, 1);
    f.reconfigure("replacement").unwrap();
    f.pump.set_required_matches(3).unwrap();
    for _ in 0..4 {
        assert_eq!(f.tick().unwrap().submitted, 0);
    }
    assert_eq!(f.parser.lock().unwrap().requests.len(), 1);
}

#[test]
fn removals_reset_samples_and_metadata_drain_has_a_per_tick_limit() {
    let mut f = Fixture::new(4, 4);
    f.tick().unwrap();
    f.source
        .lock()
        .unwrap()
        .batch
        .removed_paths
        .push("a.zevtc".into());
    assert_eq!(f.tick().unwrap().submitted, 1);
    f.source.lock().unwrap().batch.removed_paths.clear();
    assert_eq!(f.tick().unwrap().submitted, 1);
    f.complete(0);
    f.complete(1);
    assert_eq!(f.tick().unwrap().metadata.consumed, 1);
    assert_eq!(f.parser.lock().unwrap().results.len(), 1);
    assert_eq!(f.tick().unwrap().metadata.consumed, 1);
}

#[test]
fn source_errors_panics_cancellation_and_shutdown_remain_contained() {
    let mut f = Fixture::new(4, 4);
    f.source.lock().unwrap().error = Some(CandidateError::ScanFailed);
    assert_eq!(f.tick(), Err(PumpError::Source(CandidateError::ScanFailed)));
    f.source.lock().unwrap().panic = true;
    assert_eq!(f.tick(), Err(PumpError::Source(CandidateError::Panicked)));
    f.stop.request_stop();
    assert_eq!(f.tick(), Err(PumpError::Source(CandidateError::Cancelled)));
    assert_eq!(f.source.lock().unwrap().polls, 2);
    f.pump.cancel_all();
    f.pump.cancel_all();
    assert_eq!(f.tick(), Err(PumpError::ShuttingDown));
    drop(f.pump);
    assert_eq!(f.parser.lock().unwrap().stopped, 1);
}
