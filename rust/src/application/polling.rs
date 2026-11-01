use super::StopToken;
use std::time::{Duration, Instant};

/// Monotonic provider polling. Waiting is interruptible and must run outside render callbacks.
pub trait PollClock: Send + Sync {
    fn now(&self) -> Instant;
    /// Returns true when cancellation interrupts the wait.
    fn wait(&self, delay: Duration, stop: &StopToken) -> bool;
}
pub struct SystemPollClock;
impl PollClock for SystemPollClock {
    fn now(&self) -> Instant {
        Instant::now()
    }
    fn wait(&self, delay: Duration, stop: &StopToken) -> bool {
        stop.wait_for_stop(delay)
    }
}
