use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::Duration;

const CLOSED: usize = 1 << (usize::BITS - 1);

/// Lifecycle operations are serialized by the owner. Callback admission never waits for that owner.
pub struct CallbackGate {
    state: AtomicUsize,
    drained: Condvar,
    waiter: Mutex<()>,
}

impl Default for CallbackGate {
    fn default() -> Self {
        Self::new()
    }
}

impl CallbackGate {
    pub const fn new() -> Self {
        Self {
            state: AtomicUsize::new(CLOSED),
            drained: Condvar::new(),
            waiter: Mutex::new(()),
        }
    }

    pub fn open(&self) -> bool {
        self.state
            .compare_exchange(CLOSED, 0, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }

    pub fn enter(&self) -> Option<CallbackAdmission<'_>> {
        let mut state = self.state.load(Ordering::Acquire);
        loop {
            if state >= CLOSED - 1 {
                return None;
            }
            match self.state.compare_exchange_weak(
                state,
                state + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Some(CallbackAdmission(self)),
                Err(current) => state = current,
            }
        }
    }

    pub fn close(&self) {
        self.state.fetch_or(CLOSED, Ordering::AcqRel);
    }

    /// Call only after close, from the lifecycle owner, never from an admitted callback.
    pub fn wait(&self) {
        let mut waiter = self
            .waiter
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        while self.state.load(Ordering::Acquire) != CLOSED {
            // Callbacks never acquire the waiter's mutex. A final notification can therefore race
            // this wait. The timeout bounds that lost wakeup without blocking render callbacks.
            (waiter, _) = self
                .drained
                .wait_timeout(waiter, Duration::from_millis(10))
                .unwrap_or_else(|error| error.into_inner());
        }
    }
}

pub struct CallbackAdmission<'a>(&'a CallbackGate);

impl Drop for CallbackAdmission<'_> {
    fn drop(&mut self) {
        if self.0.state.fetch_sub(1, Ordering::AcqRel) == CLOSED + 1 {
            self.0.drained.notify_all();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc::{RecvTimeoutError, channel};
    use std::time::Duration;

    #[test]
    fn admission_is_closed_until_open_and_survives_repeated_cycles() {
        let gate = CallbackGate::default();
        assert!(gate.enter().is_none());
        for _ in 0..10 {
            assert!(gate.open());
            assert!(!gate.open());
            let admission = gate.enter().unwrap();
            gate.close();
            assert!(gate.enter().is_none());
            assert!(!gate.open());
            drop(admission);
            gate.wait();
            gate.close();
            gate.wait();
        }
    }

    #[test]
    fn draining_waits_for_every_admitted_callback() {
        let gate = CallbackGate::default();
        assert!(gate.open());
        let first = gate.enter().unwrap();
        let second = gate.enter().unwrap();
        gate.close();
        std::thread::scope(|scope| {
            let (started, starting) = channel();
            let (finished, completion) = channel();
            let gate = &gate;
            scope.spawn(move || {
                started.send(()).unwrap();
                gate.wait();
                finished.send(()).unwrap();
            });
            starting.recv().unwrap();
            drop(first);
            assert_eq!(
                completion.recv_timeout(Duration::from_millis(50)),
                Err(RecvTimeoutError::Timeout)
            );
            drop(second);
            completion.recv_timeout(Duration::from_secs(2)).unwrap();
        });
    }

    #[test]
    fn unwinding_releases_admission() {
        let gate = CallbackGate::default();
        gate.open();
        let result = std::panic::catch_unwind(|| {
            let _admission = gate.enter().unwrap();
            gate.close();
            panic!("synthetic callback failure");
        });
        assert!(result.is_err());
        gate.wait();
        assert!(gate.open());
    }
}
