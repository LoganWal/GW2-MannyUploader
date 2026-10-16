use std::collections::VecDeque;
use std::io;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::application::{
    CancellationSource, ProviderError, StopToken, UploadOutcome, UploadProvider, UploadRequest,
    UploadResult,
};
use crate::domain::Provider;

const MAX_PARALLELISM: usize = 32;

/// Protocol-specific work runs outside the queue lock and must cooperate with cancellation.
/// The pool constructs result identity from the admitted request.
pub trait UploadProcessor: Send + Sync + 'static {
    fn process(&self, request: &UploadRequest, stop: &StopToken) -> UploadOutcome;
}

impl<F> UploadProcessor for F
where
    F: Fn(&UploadRequest, &StopToken) -> UploadOutcome + Send + Sync + 'static,
{
    fn process(&self, request: &UploadRequest, stop: &StopToken) -> UploadOutcome {
        self(request, stop)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WorkerOptions {
    pub queue_capacity: usize,
    pub parallelism: usize,
}

impl Default for WorkerOptions {
    fn default() -> Self {
        Self {
            queue_capacity: 8,
            parallelism: 1,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WorkerError {
    InvalidCapacity,
    InvalidParallelism,
    AllocationFailed,
    ThreadStartFailed,
    Stopping,
}

impl std::fmt::Display for WorkerError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for WorkerError {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WorkerSnapshot {
    pub pending: usize,
    pub results: usize,
    /// Includes completed attempts waiting for result space.
    pub active: usize,
    pub parallelism: usize,
    pub owned_threads: usize,
    pub stopping: bool,
}

struct Queues {
    requests: VecDeque<UploadRequest>,
    results: VecDeque<UploadResult>,
    active: usize,
    parallelism: usize,
    stopping: bool,
    next_completion: usize,
    next_publication: usize,
}

struct Shared {
    queues: Mutex<Queues>,
    changed: Condvar,
    stop: CancellationSource,
    processor: Box<dyn UploadProcessor>,
    capacity: usize,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Queues> {
        self.queues
            .lock()
            .unwrap_or_else(|error| error.into_inner())
    }

    fn run(&self, index: usize) {
        let token = self.stop.token();
        loop {
            let request = {
                let queues = self.lock();
                let mut queues = self
                    .changed
                    .wait_while(queues, |queues| {
                        !queues.stopping
                            && (index >= queues.parallelism
                                || queues.active >= queues.parallelism
                                || queues.requests.is_empty())
                    })
                    .unwrap_or_else(|error| error.into_inner());
                if queues.stopping {
                    return;
                }
                queues.active += 1;
                queues.requests.pop_front().expect("admitted request")
            };
            let outcome = catch_unwind(AssertUnwindSafe(|| {
                self.processor.process(&request, &token)
            }))
            .unwrap_or(UploadOutcome::Failed);
            let result = UploadResult {
                job_id: request.job_id,
                provider: request.provider,
                attempt: request.attempt,
                outcome,
            };
            let mut queues = self.lock();
            // At most 32 tickets are outstanding. Equality remains valid across counter wrap.
            // Tickets keep earlier completions ahead of later ones during result backpressure.
            let ticket = queues.next_completion;
            queues.next_completion = queues.next_completion.wrapping_add(1);
            self.changed.notify_all();
            queues = self
                .changed
                .wait_while(queues, |queues| {
                    !queues.stopping
                        && (queues.results.len() == self.capacity
                            || ticket != queues.next_publication)
                })
                .unwrap_or_else(|error| error.into_inner());
            queues.active -= 1;
            if queues.stopping {
                self.changed.notify_all();
                return;
            }
            queues.results.push_back(result);
            queues.next_publication = queues.next_publication.wrapping_add(1);
            self.changed.notify_all();
        }
    }
}

/// One bounded, independently sized pool per provider. Drop cancels and joins all threads.
/// Construction and resizing belong to the application owner, never a Nexus callback.
pub struct AsyncUploadWorker {
    provider: Provider,
    shared: Arc<Shared>,
    threads: Vec<JoinHandle<()>>,
}

impl AsyncUploadWorker {
    pub fn new(
        provider: Provider,
        processor: impl UploadProcessor,
        options: WorkerOptions,
    ) -> Result<Self, WorkerError> {
        Self::new_with(provider, processor, options, Self::spawn)
    }

    fn new_with(
        provider: Provider,
        processor: impl UploadProcessor,
        options: WorkerOptions,
        spawn: impl FnMut(Arc<Shared>, usize) -> io::Result<JoinHandle<()>>,
    ) -> Result<Self, WorkerError> {
        if options.queue_capacity == 0 {
            return Err(WorkerError::InvalidCapacity);
        }
        Self::validate_parallelism(options.parallelism)?;
        let mut requests = VecDeque::new();
        let mut results = VecDeque::new();
        let mut threads = Vec::new();
        // All queue and handle storage is reserved before any thread starts. Queue operations
        // cannot allocate later, including when the result queue applies backpressure.
        requests
            .try_reserve_exact(options.queue_capacity)
            .map_err(|_| WorkerError::AllocationFailed)?;
        results
            .try_reserve_exact(options.queue_capacity)
            .map_err(|_| WorkerError::AllocationFailed)?;
        threads
            .try_reserve_exact(MAX_PARALLELISM)
            .map_err(|_| WorkerError::AllocationFailed)?;
        let mut worker = Self {
            provider,
            shared: Arc::new(Shared {
                queues: Mutex::new(Queues {
                    requests,
                    results,
                    active: 0,
                    parallelism: 0,
                    stopping: false,
                    next_completion: 0,
                    next_publication: 0,
                }),
                changed: Condvar::new(),
                stop: CancellationSource::default(),
                processor: Box::new(processor),
                capacity: options.queue_capacity,
            }),
            threads,
        };
        worker.resize_with(options.parallelism, spawn)?;
        Ok(worker)
    }

    pub fn update_parallelism(&mut self, parallelism: usize) -> Result<(), WorkerError> {
        self.resize_with(parallelism, Self::spawn)
    }

    fn resize_with(
        &mut self,
        parallelism: usize,
        mut spawn: impl FnMut(Arc<Shared>, usize) -> io::Result<JoinHandle<()>>,
    ) -> Result<(), WorkerError> {
        Self::validate_parallelism(parallelism)?;
        let mut queues = self.shared.lock();
        if queues.stopping {
            return Err(WorkerError::Stopping);
        }
        while self.threads.len() < parallelism {
            let thread = spawn(self.shared.clone(), self.threads.len())
                .map_err(|_| WorkerError::ThreadStartFailed)?;
            self.threads.push(thread);
        }
        // Publish only after every new thread is owned. Partial growth stays dormant on failure.
        queues.parallelism = parallelism;
        self.shared.changed.notify_all();
        Ok(())
    }

    fn spawn(shared: Arc<Shared>, index: usize) -> io::Result<JoinHandle<()>> {
        thread::Builder::new()
            .name("manny-upload".into())
            .spawn(move || shared.run(index))
    }

    fn validate_parallelism(parallelism: usize) -> Result<(), WorkerError> {
        if (1..=MAX_PARALLELISM).contains(&parallelism) {
            Ok(())
        } else {
            Err(WorkerError::InvalidParallelism)
        }
    }

    pub fn snapshot(&self) -> WorkerSnapshot {
        let queues = self.shared.lock();
        WorkerSnapshot {
            pending: queues.requests.len(),
            results: queues.results.len(),
            active: queues.active,
            parallelism: queues.parallelism,
            owned_threads: self.threads.len(),
            stopping: queues.stopping,
        }
    }

    /// Blocking result wait for background consumers and tests. The coordinator uses try_take_result.
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<UploadResult> {
        let queues = self.shared.lock();
        let (mut queues, _) = self
            .shared
            .changed
            .wait_timeout_while(queues, timeout, |queues| {
                !queues.stopping && queues.results.is_empty()
            })
            .unwrap_or_else(|error| error.into_inner());
        let result = queues.results.pop_front();
        self.shared.changed.notify_all();
        result
    }
}

impl UploadProvider for AsyncUploadWorker {
    fn provider(&self) -> Provider {
        self.provider
    }

    fn enqueue(&mut self, request: UploadRequest) -> Result<(), ProviderError> {
        if request.provider != self.provider
            || request.attempt == 0
            || request.file.canonical_path.as_os_str().is_empty()
        {
            return Err(ProviderError::InvalidRequest);
        }
        let mut queues = self.shared.lock();
        if queues.stopping {
            return Err(ProviderError::Stopping);
        }
        if queues.requests.len() == self.shared.capacity {
            return Err(ProviderError::QueueFull);
        }
        queues.requests.push_back(request);
        self.shared.changed.notify_all();
        Ok(())
    }

    fn try_take_result(&mut self) -> Result<Option<UploadResult>, ProviderError> {
        let result = self.shared.lock().results.pop_front();
        self.shared.changed.notify_all();
        Ok(result)
    }

    fn cancel_pending(&mut self) {
        let mut queues = self.shared.lock();
        if queues.stopping {
            return;
        }
        queues.stopping = true;
        queues.requests.clear();
        queues.results.clear();
        self.shared.stop.request_stop();
        self.shared.changed.notify_all();
    }
}

impl Drop for AsyncUploadWorker {
    fn drop(&mut self) {
        self.cancel_pending();
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::UploadIntent;
    use crate::domain::{EncounterMetadata, LogFileIdentity, UploadJobId};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::time::SystemTime;

    const TIMEOUT: Duration = Duration::from_secs(5);

    fn immediate(_: &UploadRequest, _: &StopToken) -> UploadOutcome {
        UploadOutcome::Skipped
    }

    fn request(id: u64) -> UploadRequest {
        UploadRequest {
            job_id: UploadJobId::new(id).unwrap(),
            provider: Provider::Wingman,
            attempt: 1,
            file: LogFileIdentity {
                canonical_path: "logs/example.zevtc".into(),
                size: 1,
                last_write_time: SystemTime::UNIX_EPOCH,
            },
            metadata: EncounterMetadata {
                boss_id: 123,
                pov_account: "Player.1234".into(),
                remaining_health_basis_points: None,
            },
            dps_report_result: None,
            intent: UploadIntent::Automatic,
        }
    }

    fn wait_until(worker: &AsyncUploadWorker, ready: impl Fn(&Queues) -> bool) {
        let queues = worker.shared.lock();
        let (queues, _) = worker
            .shared
            .changed
            .wait_timeout_while(queues, TIMEOUT, |queues| !ready(queues))
            .unwrap();
        assert!(
            ready(&queues),
            "worker state did not reach the expected barrier"
        );
    }

    #[test]
    fn failed_construction_joins_every_partially_started_thread() {
        let exited = Arc::new(AtomicUsize::new(0));
        let counter = exited.clone();
        let worker = AsyncUploadWorker::new_with(
            Provider::Wingman,
            immediate,
            WorkerOptions {
                queue_capacity: 1,
                parallelism: 3,
            },
            move |shared, index| {
                if index == 1 {
                    return Err(io::Error::other("synthetic thread start failure"));
                }
                let counter = counter.clone();
                thread::Builder::new().spawn(move || {
                    shared.run(index);
                    counter.fetch_add(1, Ordering::Release);
                })
            },
        );
        assert!(matches!(worker, Err(WorkerError::ThreadStartFailed)));
        assert_eq!(exited.load(Ordering::Acquire), 1);
    }

    #[test]
    fn failed_growth_preserves_limit_owns_partial_threads_and_can_recover() {
        let mut worker =
            AsyncUploadWorker::new(Provider::Wingman, immediate, WorkerOptions::default()).unwrap();
        let exited = Arc::new(AtomicUsize::new(0));
        let counter = exited.clone();
        let updated = worker.resize_with(4, move |shared, index| {
            if index == 2 {
                return Err(io::Error::other("synthetic thread start failure"));
            }
            let counter = counter.clone();
            thread::Builder::new().spawn(move || {
                shared.run(index);
                counter.fetch_add(1, Ordering::Release);
            })
        });
        assert_eq!(updated, Err(WorkerError::ThreadStartFailed));
        assert_eq!(worker.snapshot().parallelism, 1);
        assert_eq!(worker.snapshot().owned_threads, 2);
        worker.enqueue(request(1)).unwrap();
        assert_eq!(worker.wait_for_result(TIMEOUT).unwrap().job_id.get(), 1);
        worker.update_parallelism(4).unwrap();
        assert_eq!(worker.snapshot().owned_threads, 4);
        worker.update_parallelism(1).unwrap();
        drop(worker);
        assert_eq!(exited.load(Ordering::Acquire), 1);
    }

    #[test]
    fn result_backpressure_preserves_ticket_order_across_counter_wrap() {
        let mut worker = AsyncUploadWorker::new(
            Provider::Wingman,
            immediate,
            WorkerOptions {
                queue_capacity: 1,
                parallelism: 3,
            },
        )
        .unwrap();
        {
            let mut queues = worker.shared.lock();
            queues.next_completion = usize::MAX;
            queues.next_publication = usize::MAX;
        }
        worker.enqueue(request(1)).unwrap();
        wait_until(&worker, |queues| queues.results.len() == 1);
        worker.enqueue(request(2)).unwrap();
        wait_until(&worker, |queues| queues.next_completion == 1);
        worker.enqueue(request(3)).unwrap();
        wait_until(&worker, |queues| queues.next_completion == 2);
        assert_eq!(worker.snapshot().active, 2);
        for id in 1..=3 {
            assert_eq!(worker.wait_for_result(TIMEOUT).unwrap().job_id.get(), id);
        }
    }

    #[test]
    fn cancellation_wakes_every_result_publisher_after_it_has_blocked() {
        let mut worker = AsyncUploadWorker::new(
            Provider::Wingman,
            immediate,
            WorkerOptions {
                queue_capacity: 1,
                parallelism: 2,
            },
        )
        .unwrap();
        worker.enqueue(request(1)).unwrap();
        wait_until(&worker, |queues| queues.results.len() == 1);
        worker.enqueue(request(2)).unwrap();
        wait_until(&worker, |queues| queues.next_completion == 2);
        worker.enqueue(request(3)).unwrap();
        wait_until(&worker, |queues| queues.next_completion == 3);
        worker.enqueue(request(4)).unwrap();
        worker.cancel_pending();
        wait_until(&worker, |queues| queues.active == 0);
        assert_eq!(worker.snapshot().pending, 0);
        assert!(worker.try_take_result().unwrap().is_none());
    }
}
