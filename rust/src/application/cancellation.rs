use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

#[derive(Default)]
struct StopState {
    requested: AtomicBool,
    mutex: Mutex<()>,
    changed: Condvar,
}

/// Only the owner can request cancellation. Processors receive a read-only token.
#[derive(Default)]
pub struct CancellationSource {
    state: Arc<StopState>,
}

impl CancellationSource {
    pub fn token(&self) -> StopToken {
        StopToken {
            state: self.state.clone(),
        }
    }

    pub fn request_stop(&self) {
        // Pair the flag change with the wait mutex so a notification cannot be lost.
        let _guard = self
            .state
            .mutex
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        self.state.requested.store(true, Ordering::Release);
        self.state.changed.notify_all();
    }
}

#[derive(Clone)]
pub struct StopToken {
    state: Arc<StopState>,
}

impl StopToken {
    pub fn is_stop_requested(&self) -> bool {
        self.state.requested.load(Ordering::Acquire)
    }

    /// Interruptible blocking wait for processors, never for render callbacks.
    /// Returns true if cancellation was requested, including before this call.
    pub fn wait_for_stop(&self, timeout: Duration) -> bool {
        let guard = self
            .state
            .mutex
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let _guard = self
            .state
            .changed
            .wait_timeout_while(guard, timeout, |_| !self.is_stop_requested())
            .unwrap_or_else(|error| error.into_inner());
        self.is_stop_requested()
    }
}
