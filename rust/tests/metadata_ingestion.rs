use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::evtc::{MetadataParserWorker, WorkerError};

const TIMEOUT: Duration = Duration::from_secs(5);

fn file(size: u64) -> LogFileIdentity {
    LogFileIdentity {
        canonical_path: "logs/example.zevtc".into(),
        size,
        last_write_time: SystemTime::UNIX_EPOCH,
    }
}

fn metadata() -> EncounterMetadata {
    EncounterMetadata {
        boss_id: 123,
        pov_account: ":Player.1234".into(),
        remaining_health_basis_points: Some(1234),
    }
}

fn request(id: u64) -> MetadataRequest {
    MetadataRequest {
        job_id: UploadJobId::new(id).unwrap(),
        file: file(id),
    }
}
fn selected() -> ProviderSelection {
    ProviderSelection {
        dps_report: true,
        ..Default::default()
    }
}
fn read(_: &LogFileIdentity, _: &StopToken) -> Result<EncounterMetadata, MetadataError> {
    Ok(metadata())
}

#[test]
fn parser_validates_capacity_and_requests() {
    assert!(matches!(
        MetadataParserWorker::new(read, 0),
        Err(WorkerError::InvalidCapacity)
    ));
    let mut parser = MetadataParserWorker::new(read, 2).unwrap();
    let mut invalid = request(1);
    invalid.file.canonical_path.clear();
    assert_eq!(parser.enqueue(invalid), Err(ParserError::InvalidRequest));
    assert_eq!(parser.available_capacity(), 2);
    assert_eq!(parser.snapshot().parallelism, 1);
    parser.cancel_pending();
    assert_eq!(parser.available_capacity(), 0);
    assert_eq!(parser.enqueue(request(1)), Err(ParserError::Stopping));
}

#[test]
fn live_parser_capacity_preserves_queued_work_while_restricting_admission() {
    let (started, calls) = mpsc::channel();
    let (release, wait) = mpsc::channel();
    let wait = Mutex::new(wait);
    let mut parser = MetadataParserWorker::new(
        move |file: &LogFileIdentity, _: &StopToken| {
            started.send(file.size).unwrap();
            wait.lock().unwrap().recv_timeout(TIMEOUT).unwrap();
            Ok(metadata())
        },
        3,
    )
    .unwrap();
    parser.enqueue(request(1)).unwrap();
    assert_eq!(calls.recv_timeout(TIMEOUT).unwrap(), 1);
    for id in 2..=4 {
        parser.enqueue(request(id)).unwrap();
    }
    parser.update_queue_capacity(1).unwrap();
    assert_eq!(parser.available_capacity(), 0);
    assert_eq!(parser.enqueue(request(5)), Err(ParserError::QueueFull));
    for id in 1..=4 {
        release.send(()).unwrap();
        assert_eq!(parser.wait_for_result(TIMEOUT).unwrap().job_id.get(), id);
        if id < 4 {
            assert_eq!(calls.recv_timeout(TIMEOUT).unwrap(), id + 1);
        }
    }
    assert_eq!(parser.available_capacity(), 1);
    for invalid in [0, 65, usize::MAX] {
        assert_eq!(
            parser.update_queue_capacity(invalid),
            Err(ParserError::InvalidRequest)
        );
        assert_eq!(parser.available_capacity(), 1);
    }
    parser.update_queue_capacity(64).unwrap();
    assert_eq!(parser.available_capacity(), 64);
    parser.cancel_pending();
    assert_eq!(parser.update_queue_capacity(2), Err(ParserError::Stopping));
}

#[test]
fn parser_preserves_fifo_ids_errors_and_contains_reader_panics() {
    let mut parser = MetadataParserWorker::new(
        |file: &LogFileIdentity, _: &StopToken| match file.size {
            1 => Ok(metadata()),
            2 => panic!("synthetic reader failure"),
            3 => Err(MetadataError::MalformedLog),
            _ => Err(MetadataError::Cancelled),
        },
        4,
    )
    .unwrap();
    for id in 1..=4 {
        parser.enqueue(request(id)).unwrap();
    }
    for (index, expected) in [
        Ok(metadata()),
        Err(MetadataError::Internal),
        Err(MetadataError::MalformedLog),
        Err(MetadataError::Cancelled),
    ]
    .into_iter()
    .enumerate()
    {
        let result = parser.wait_for_result(TIMEOUT).unwrap();
        assert_eq!(result.job_id.get(), index as u64 + 1);
        assert_eq!(result.metadata, expected);
    }
}

#[test]
fn metadata_parser_bounds_input_and_result_queues() {
    let (started, calls) = mpsc::sync_channel(2);
    let mut parser = MetadataParserWorker::new(
        move |file: &LogFileIdentity, _: &StopToken| {
            let (release, response) = mpsc::sync_channel(1);
            started.send((file.size, release)).unwrap();
            response
                .recv_timeout(TIMEOUT)
                .unwrap_or(Err(MetadataError::Cancelled))
        },
        1,
    )
    .unwrap();
    parser.enqueue(request(1)).unwrap();
    let (id, first) = calls.recv_timeout(TIMEOUT).unwrap();
    assert_eq!(id, 1);
    parser.enqueue(request(2)).unwrap();
    assert_eq!(parser.available_capacity(), 0);
    assert_eq!(parser.enqueue(request(3)), Err(ParserError::QueueFull));
    first.send(Ok(metadata())).unwrap();
    let (id, second) = calls.recv_timeout(TIMEOUT).unwrap();
    assert_eq!(id, 2);
    second.send(Ok(metadata())).unwrap();
    parser.enqueue(request(3)).unwrap();
    assert_eq!(
        (parser.snapshot().results, parser.snapshot().pending),
        (1, 1)
    );
    assert_eq!(parser.wait_for_result(TIMEOUT).unwrap().job_id.get(), 1);
    let (id, third) = calls.recv_timeout(TIMEOUT).unwrap();
    assert_eq!(id, 3);
    third.send(Ok(metadata())).unwrap();
    assert_eq!(parser.wait_for_result(TIMEOUT).unwrap().job_id.get(), 2);
    assert_eq!(parser.wait_for_result(TIMEOUT).unwrap().job_id.get(), 3);
}

struct TestClock;

#[test]
fn ingestion_owner_routes_capacity_changes_contains_panics_and_stops_reconfiguration() {
    struct Parser(Arc<AtomicUsize>);
    impl LogMetadataParser for Parser {
        fn update_queue_capacity(&mut self, capacity: usize) -> Result<(), ParserError> {
            if capacity == 65 {
                panic!("synthetic capacity failure");
            }
            if capacity == 0 {
                return Err(ParserError::InvalidRequest);
            }
            self.0.store(capacity, Ordering::SeqCst);
            Ok(())
        }
        fn enqueue(&mut self, _: MetadataRequest) -> Result<(), ParserError> {
            panic!("no admission expected")
        }
        fn available_capacity(&self) -> usize {
            self.0.load(Ordering::SeqCst)
        }
        fn try_take_result(&mut self) -> Result<Option<MetadataResult>, ParserError> {
            Ok(None)
        }
        fn cancel_pending(&mut self) {}
    }
    let capacity = Arc::new(AtomicUsize::new(8));
    let uploads = UploadCoordinator::new(TestClock, vec![], 8).unwrap();
    let mut ingestion = LogIngestionCoordinator::new(uploads, Box::new(Parser(capacity.clone())));
    ingestion.update_parser_capacity(16).unwrap();
    assert_eq!(ingestion.available_capacity(), Ok(16));
    assert_eq!(
        ingestion.update_parser_capacity(0),
        Err(IngestionError::Parser(ParserError::InvalidRequest))
    );
    assert_eq!(
        ingestion.update_parser_capacity(65),
        Err(IngestionError::Parser(ParserError::Panicked))
    );
    assert_eq!(capacity.load(Ordering::SeqCst), 16);
    ingestion.cancel_all();
    assert_eq!(
        ingestion.update_parser_capacity(2),
        Err(IngestionError::ShuttingDown)
    );
    assert_eq!(capacity.load(Ordering::SeqCst), 16);
}
impl Clock for TestClock {
    fn system_now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }
    fn steady_now(&self) -> Instant {
        Instant::now()
    }
}

struct ParserState {
    capacity: usize,
    requests: Vec<MetadataRequest>,
    results: VecDeque<MetadataResult>,
    reject: bool,
    panic_enqueue: bool,
    panic_poll: bool,
    panic_cancel: bool,
    cancellations: usize,
}

impl Default for ParserState {
    fn default() -> Self {
        Self {
            capacity: 8,
            requests: Vec::new(),
            results: VecDeque::new(),
            reject: false,
            panic_enqueue: false,
            panic_poll: false,
            panic_cancel: false,
            cancellations: 0,
        }
    }
}

struct FakeParser(Arc<Mutex<ParserState>>);
impl LogMetadataParser for FakeParser {
    fn available_capacity(&self) -> usize {
        self.0.lock().unwrap().capacity
    }
    fn enqueue(&mut self, request: MetadataRequest) -> Result<(), ParserError> {
        let mut state = self.0.lock().unwrap();
        if state.panic_enqueue {
            drop(state);
            panic!("synthetic parser enqueue failure");
        }
        if state.reject {
            return Err(ParserError::Unavailable);
        }
        state.requests.push(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Result<Option<MetadataResult>, ParserError> {
        let mut state = self.0.lock().unwrap();
        if state.panic_poll {
            drop(state);
            panic!("synthetic parser polling failure");
        }
        Ok(state.results.pop_front())
    }
    fn cancel_pending(&mut self) {
        let mut state = self.0.lock().unwrap();
        state.cancellations += 1;
        if state.panic_cancel {
            drop(state);
            panic!("synthetic parser cancellation failure");
        }
        state.results.clear();
    }
}

struct FakeProvider {
    requests: Arc<Mutex<Vec<UploadRequest>>>,
    cancellations: Arc<AtomicUsize>,
}
impl UploadProvider for FakeProvider {
    fn provider(&self) -> Provider {
        Provider::DpsReport
    }
    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        self.requests.lock().unwrap().push(request);
        Ok(())
    }
    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        Ok(None)
    }
    fn cancel_pending(&mut self) {
        self.cancellations.fetch_add(1, Ordering::Relaxed);
    }
}

struct Fixture {
    ingestion: LogIngestionCoordinator<TestClock>,
    parser: Arc<Mutex<ParserState>>,
    uploads: Arc<Mutex<Vec<UploadRequest>>>,
    provider_stops: Arc<AtomicUsize>,
}

impl Fixture {
    fn new(limit: usize) -> Self {
        let parser = Arc::new(Mutex::new(ParserState::default()));
        let uploads = Arc::new(Mutex::new(Vec::new()));
        let provider_stops = Arc::new(AtomicUsize::new(0));
        let provider = FakeProvider {
            requests: uploads.clone(),
            cancellations: provider_stops.clone(),
        };
        let coordinator =
            UploadCoordinator::new(TestClock, vec![Box::new(provider)], limit).unwrap();
        Self {
            ingestion: LogIngestionCoordinator::new(
                coordinator,
                Box::new(FakeParser(parser.clone())),
            ),
            parser,
            uploads,
            provider_stops,
        }
    }
    fn state(&self, id: UploadJobId) -> ProviderState {
        self.ingestion
            .uploads()
            .snapshots()
            .into_iter()
            .find(|job| job.id == id)
            .unwrap()
            .record
            .providers[0]
            .state
    }
    fn complete(&self, id: UploadJobId, metadata: Result<EncounterMetadata, MetadataError>) {
        self.parser
            .lock()
            .unwrap()
            .results
            .push_back(MetadataResult {
                job_id: id,
                metadata,
            });
    }
}

#[test]
fn ingestion_waits_for_metadata_and_starts_only_the_correlated_job() {
    let mut fixture = Fixture::new(5);
    let id = fixture.ingestion.submit(file(1), selected()).unwrap();
    let second = fixture.ingestion.submit(file(2), selected()).unwrap();
    assert_eq!(fixture.state(id), ProviderState::Waiting);
    assert!(fixture.uploads.lock().unwrap().is_empty());
    fixture.complete(second, Ok(metadata()));
    assert_eq!(fixture.ingestion.drain_metadata(1).consumed, 1);
    assert_eq!(fixture.state(id), ProviderState::Waiting);
    assert_eq!(fixture.state(second), ProviderState::Active);
    assert_eq!(fixture.uploads.lock().unwrap()[0].job_id, second);
    assert_eq!(fixture.uploads.lock().unwrap()[0].metadata, metadata());
}

#[test]
fn parser_backpressure_does_not_create_failed_rows_or_consume_ids() {
    let mut fixture = Fixture::new(1);
    fixture.parser.lock().unwrap().capacity = 0;
    assert_eq!(
        fixture.ingestion.submit(file(1), selected()),
        Err(IngestionError::ParserFull)
    );
    assert!(fixture.ingestion.uploads().snapshots().is_empty());
    fixture.parser.lock().unwrap().capacity = 1;
    let id = fixture.ingestion.submit(file(1), selected()).unwrap();
    assert_eq!(id.get(), 1);
    assert_eq!(
        fixture.ingestion.submit(file(2), selected()),
        Err(IngestionError::Coordinator(
            CoordinatorError::CapacityReached
        ))
    );
    assert_eq!(fixture.parser.lock().unwrap().requests.len(), 1);
}

#[test]
fn unexpected_dispatch_rejection_or_panic_retains_an_accepted_failed_job() {
    for panic in [false, true] {
        let mut fixture = Fixture::new(5);
        fixture.parser.lock().unwrap().reject = true;
        fixture.parser.lock().unwrap().panic_enqueue = panic;
        let id = fixture.ingestion.submit(file(1), selected()).unwrap();
        assert_eq!(fixture.state(id), ProviderState::Failed);
        assert_eq!(
            fixture.ingestion.handle_result(MetadataResult {
                job_id: id,
                metadata: Ok(metadata())
            }),
            Err(IngestionError::UnexpectedResult)
        );
        assert!(fixture.uploads.lock().unwrap().is_empty());
    }
}

#[test]
fn metadata_errors_and_cancellation_settle_pending_jobs_without_uploading() {
    let mut fixture = Fixture::new(5);
    for (error, state) in [
        (MetadataError::MalformedLog, ProviderState::Failed),
        (MetadataError::Cancelled, ProviderState::Cancelled),
        (MetadataError::Internal, ProviderState::Failed),
    ] {
        let id = fixture.ingestion.submit(file(1), selected()).unwrap();
        fixture.complete(id, Err(error));
        assert_eq!(
            fixture.ingestion.drain_metadata(1),
            MetadataDrainSummary {
                consumed: 1,
                ..Default::default()
            }
        );
        assert_eq!(fixture.state(id), state);
    }
    assert!(fixture.uploads.lock().unwrap().is_empty());
}

#[test]
fn drain_budget_rejects_unknown_and_duplicate_completions() {
    let mut fixture = Fixture::new(5);
    let id = fixture.ingestion.submit(file(1), selected()).unwrap();
    fixture.complete(UploadJobId::new(999).unwrap(), Ok(metadata()));
    fixture.complete(id, Ok(metadata()));
    fixture.complete(id, Ok(metadata()));
    assert_eq!(
        fixture.ingestion.drain_metadata(0),
        MetadataDrainSummary::default()
    );
    assert_eq!(
        fixture.ingestion.drain_metadata(2),
        MetadataDrainSummary {
            consumed: 2,
            rejected: 1,
            parser_errors: 0
        }
    );
    assert_eq!(
        fixture.ingestion.drain_metadata(1),
        MetadataDrainSummary {
            consumed: 1,
            rejected: 1,
            parser_errors: 0
        }
    );
    assert_eq!(fixture.uploads.lock().unwrap().len(), 1);
}

#[test]
fn cancellation_contains_parser_panics_and_stops_provider_work_once() {
    let mut fixture = Fixture::new(5);
    let id = fixture.ingestion.submit(file(1), selected()).unwrap();
    fixture.parser.lock().unwrap().panic_poll = true;
    assert_eq!(fixture.ingestion.drain_metadata(10).parser_errors, 1);
    fixture.parser.lock().unwrap().panic_cancel = true;
    fixture.ingestion.cancel_all();
    fixture.ingestion.cancel_all();
    assert_eq!(fixture.state(id), ProviderState::Cancelled);
    assert_eq!(fixture.ingestion.available_capacity(), Ok(0));
    assert_eq!(
        fixture.ingestion.submit(file(2), selected()),
        Err(IngestionError::ShuttingDown)
    );
    assert_eq!(
        fixture.ingestion.handle_result(MetadataResult {
            job_id: id,
            metadata: Ok(metadata())
        }),
        Err(IngestionError::ShuttingDown)
    );
    assert_eq!(
        fixture.ingestion.drain_metadata(5),
        MetadataDrainSummary::default()
    );
    let parser = fixture.parser.clone();
    let stops = fixture.provider_stops.clone();
    drop(fixture);
    assert_eq!(parser.lock().unwrap().cancellations, 1);
    assert_eq!(stops.load(Ordering::Relaxed), 1);
}

#[test]
fn dropping_ingestion_cancels_and_joins_an_active_metadata_reader() {
    let (started, ready) = mpsc::sync_channel(1);
    let stopped = Arc::new(AtomicBool::new(false));
    let observed = stopped.clone();
    let parser = MetadataParserWorker::new(
        move |_: &LogFileIdentity, stop: &StopToken| {
            started.send(()).unwrap();
            observed.store(stop.wait_for_stop(TIMEOUT), Ordering::Release);
            Err(MetadataError::Cancelled)
        },
        1,
    )
    .unwrap();
    let uploads = UploadCoordinator::new(TestClock, Vec::new(), 1).unwrap();
    let mut ingestion = LogIngestionCoordinator::new(uploads, Box::new(parser));
    ingestion
        .submit(file(1), ProviderSelection::default())
        .unwrap();
    ready.recv_timeout(TIMEOUT).unwrap();
    drop(ingestion);
    assert!(stopped.load(Ordering::Acquire));
}
