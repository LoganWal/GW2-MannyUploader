use super::TwitchChatDelivery;
use crate::{
    application::*,
    providers::{WorkerError, WorkerOptions, WorkerSnapshot},
    workers::{Processor, QueueError, WorkerPool},
};
use std::{sync::Arc, time::Duration};

struct Captured {
    id: u64,
    entered: bool,
}
struct SendTest(TwitchChatDelivery);
impl Processor for SendTest {
    type Request = Captured;
    type Output = TwitchTestMessageResult;
    fn process(&self, request: &mut Captured, stop: &StopToken) -> TwitchTestMessageResult {
        if stop.is_stop_requested() {
            return TwitchTestMessageResult {
                request_id: request.id,
                outcome: TwitchTestMessageOutcome::Cancelled,
            };
        }
        let message = format!("GW2 Manny Uploader test message #{}", request.id);
        request.entered = true;
        let outcome = match self.0.send(&message, stop) {
            TwitchChatResult::Sent(_) => TwitchTestMessageOutcome::Sent,
            TwitchChatResult::Dropped(receipt) => TwitchTestMessageOutcome::Dropped(receipt.status),
            TwitchChatResult::RetryAfter(delay) => TwitchTestMessageOutcome::RetryAfter(delay),
            TwitchChatResult::Failed(error) => TwitchTestMessageOutcome::Failed(error),
            TwitchChatResult::Ambiguous => TwitchTestMessageOutcome::Ambiguous,
            TwitchChatResult::Cancelled => TwitchTestMessageOutcome::Cancelled,
        };
        TwitchTestMessageResult {
            request_id: request.id,
            outcome,
        }
    }
    fn failed(&self, request: &Captured) -> TwitchTestMessageResult {
        TwitchTestMessageResult {
            request_id: request.id,
            outcome: if request.entered {
                TwitchTestMessageOutcome::Ambiguous
            } else {
                TwitchTestMessageOutcome::Failed(TwitchChatError::SendFailed)
            },
        }
    }
}
pub struct TwitchTestMessageWorker {
    pool: WorkerPool<SendTest>,
}
impl TwitchTestMessageWorker {
    pub fn new(
        client: Arc<dyn TwitchApi>,
        sessions: Arc<dyn TwitchDeliverySessionAccess>,
        queue_capacity: usize,
    ) -> Result<Self, WorkerError> {
        Ok(Self {
            pool: WorkerPool::new(
                SendTest(TwitchChatDelivery::new(client, sessions)),
                WorkerOptions {
                    queue_capacity,
                    parallelism: 1,
                },
            )?,
        })
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<TwitchTestMessageResult> {
        self.pool.wait_for_result(timeout)
    }
}
impl TwitchTestMessenger for TwitchTestMessageWorker {
    fn enqueue(
        &mut self,
        request: TwitchTestMessageRequest,
    ) -> Result<(), TwitchTestMessageDispatchError> {
        if request.request_id == 0 {
            return Err(TwitchTestMessageDispatchError::InvalidRequest);
        }
        self.pool
            .enqueue(Captured {
                id: request.request_id,
                entered: false,
            })
            .map_err(|error| match error {
                QueueError::Full => TwitchTestMessageDispatchError::QueueFull,
                QueueError::Stopping => TwitchTestMessageDispatchError::Stopping,
            })
    }
    fn try_take_result(&mut self) -> Option<TwitchTestMessageResult> {
        self.pool.try_take_result()
    }
    fn cancel_pending(&mut self) {
        self.pool.cancel_pending();
    }
}
