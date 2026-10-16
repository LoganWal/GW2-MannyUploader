use std::collections::VecDeque;
use std::io;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::application::{CancellationSource, StopToken};

const MAX_PARALLELISM: usize = 32;

pub(crate) trait Processor: Send + Sync + 'static {
    type Request: Send + 'static;
    type Output: Send + 'static;
    fn process(&self, request: &Self::Request, stop: &StopToken) -> Self::Output;
    fn failed(&self, request: &Self::Request) -> Self::Output;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum QueueError {
    Full,
    Stopping,
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

struct Queues<Q, R> {
    requests: VecDeque<Q>,
    results: VecDeque<R>,
    active: usize,
    parallelism: usize,
    stopping: bool,
    next_completion: usize,
    next_publication: usize,
}

struct Shared<P: Processor> {
    queues: Mutex<Queues<P::Request, P::Output>>,
    changed: Condvar,
    stop: CancellationSource,
    processor: P,
    capacity: usize,
}

impl<P: Processor> Shared<P> {
    fn lock(&self) -> MutexGuard<'_, Queues<P::Request, P::Output>> {
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
            let result = catch_unwind(AssertUnwindSafe(|| {
                self.processor.process(&request, &token)
            }))
            .unwrap_or_else(|_| self.processor.failed(&request));
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

/// One bounded, independently sized pool. Drop cancels and joins all threads.
/// Construction and resizing belong to the application owner, never a Nexus callback.
pub(crate) struct WorkerPool<P: Processor> {
    shared: Arc<Shared<P>>,
    threads: Vec<JoinHandle<()>>,
}

impl<P: Processor> WorkerPool<P> {
    pub fn new(processor: P, options: WorkerOptions) -> Result<Self, WorkerError> {
        Self::new_with(processor, options, Self::spawn)
    }

    fn new_with(
        processor: P,
        options: WorkerOptions,
        spawn: impl FnMut(Arc<Shared<P>>, usize) -> io::Result<JoinHandle<()>>,
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
                processor,
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
        mut spawn: impl FnMut(Arc<Shared<P>>, usize) -> io::Result<JoinHandle<()>>,
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

    fn spawn(shared: Arc<Shared<P>>, index: usize) -> io::Result<JoinHandle<()>> {
        thread::Builder::new()
            .name("manny-worker".into())
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
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<P::Output> {
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
    pub fn enqueue(&mut self, request: P::Request) -> Result<(), QueueError> {
        let mut queues = self.shared.lock();
        if queues.stopping {
            return Err(QueueError::Stopping);
        }
        if queues.requests.len() == self.shared.capacity {
            return Err(QueueError::Full);
        }
        queues.requests.push_back(request);
        self.shared.changed.notify_all();
        Ok(())
    }

    pub fn try_take_result(&mut self) -> Option<P::Output> {
        let result = self.shared.lock().results.pop_front();
        self.shared.changed.notify_all();
        result
    }

    pub fn available_capacity(&self) -> usize {
        let queues = self.shared.lock();
        if queues.stopping {
            0
        } else {
            self.shared.capacity - queues.requests.len()
        }
    }

    pub fn cancel_pending(&mut self) {
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

impl<P: Processor> Drop for WorkerPool<P> {
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
    use std::sync::atomic::{AtomicUsize, Ordering};

    const TIMEOUT: Duration = Duration::from_secs(5);

    struct Echo;
    impl Processor for Echo {
        type Request = u64;
        type Output = u64;
        fn process(&self, request: &u64, _: &StopToken) -> u64 {
            *request
        }
        fn failed(&self, _: &u64) -> u64 {
            0
        }
    }

    fn request(id: u64) -> u64 {
        id
    }

    fn wait_until(worker: &WorkerPool<Echo>, ready: impl Fn(&Queues<u64, u64>) -> bool) {
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
        let worker = WorkerPool::new_with(
            Echo,
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
        let mut worker = WorkerPool::new(Echo, WorkerOptions::default()).unwrap();
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
        assert_eq!(worker.wait_for_result(TIMEOUT).unwrap(), 1);
        worker.update_parallelism(4).unwrap();
        assert_eq!(worker.snapshot().owned_threads, 4);
        worker.update_parallelism(1).unwrap();
        drop(worker);
        assert_eq!(exited.load(Ordering::Acquire), 1);
    }

    #[test]
    fn result_backpressure_preserves_ticket_order_across_counter_wrap() {
        let mut worker = WorkerPool::new(
            Echo,
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
            assert_eq!(worker.wait_for_result(TIMEOUT).unwrap(), id);
        }
    }

    #[test]
    fn cancellation_wakes_every_result_publisher_after_it_has_blocked() {
        let mut worker = WorkerPool::new(
            Echo,
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
        assert!(worker.try_take_result().is_none());
    }
}
