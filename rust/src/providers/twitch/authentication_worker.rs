use crate::{
    application::*,
    providers::{WorkerError, WorkerOptions, WorkerSnapshot},
    workers::{Processor, QueueError, WorkerPool},
};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::Arc,
    time::Duration,
};

struct Captured {
    id: u64,
    operation: TwitchOperation,
    command: Option<TwitchAuthenticationCommand>,
}
enum Primitive {
    Started(TwitchDeviceAuthorization),
    Polled(TwitchDevicePollResult),
    Validated(TwitchValidatedIdentity),
    Refreshed(TwitchTokenGrant),
    Revoked,
}
struct Authenticate(Arc<dyn TwitchApi>);
impl Authenticate {
    fn call(
        &self,
        command: &TwitchAuthenticationCommand,
        stop: &StopToken,
    ) -> Result<Primitive, TwitchError> {
        match command {
            TwitchAuthenticationCommand::Start => self
                .0
                .start_device_authorization(stop)
                .map(Primitive::Started),
            TwitchAuthenticationCommand::Poll { device_code } => self
                .0
                .poll_device_authorization(device_code, stop)
                .map(Primitive::Polled),
            TwitchAuthenticationCommand::Validate { credentials } => self
                .0
                .validate_access_token(&credentials.access_token, stop)
                .map(Primitive::Validated),
            TwitchAuthenticationCommand::Refresh { credentials } => self
                .0
                .refresh_access_token(&credentials.refresh_token, stop)
                .map(Primitive::Refreshed),
            TwitchAuthenticationCommand::Revoke { access_token } => self
                .0
                .revoke_access_token(access_token, stop)
                .map(|()| Primitive::Revoked),
        }
    }
}
impl Processor for Authenticate {
    type Request = Captured;
    type Output = TwitchAuthenticationResult;
    fn process(&self, request: &mut Captured, stop: &StopToken) -> Self::Output {
        let command = request
            .command
            .take()
            .expect("owned authentication command");
        let operation = request.operation;
        let outcome = if stop.is_stop_requested() {
            Err(cancelled(operation))
        } else {
            // Keep the command outside the unwind boundary so recovery receives the same allocation.
            let result = catch_unwind(AssertUnwindSafe(|| self.call(&command, stop)))
                .unwrap_or_else(|_| Err(TwitchError::failed(operation, TwitchErrorCode::Internal)));
            if stop.is_stop_requested() {
                Err(cancelled(operation))
            } else {
                result
            }
        };
        let (outcome, retained) = match outcome {
            Err(mut error) => {
                error.operation = operation;
                let retained = match command {
                    TwitchAuthenticationCommand::Poll { device_code } => {
                        Some(TwitchAuthenticationSecret::DeviceCode(device_code))
                    }
                    TwitchAuthenticationCommand::Validate { credentials }
                    | TwitchAuthenticationCommand::Refresh { credentials } => {
                        Some(TwitchAuthenticationSecret::Credentials(credentials))
                    }
                    _ => None,
                };
                (Err(error), retained)
            }
            Ok(primitive) => {
                let success = match (command, primitive) {
                    (TwitchAuthenticationCommand::Start, Primitive::Started(value)) => {
                        TwitchAuthenticationSuccess::Started(value)
                    }
                    (
                        TwitchAuthenticationCommand::Poll { device_code },
                        Primitive::Polled(TwitchDevicePollResult::Pending),
                    ) => TwitchAuthenticationSuccess::Pending { device_code },
                    (
                        TwitchAuthenticationCommand::Poll { .. },
                        Primitive::Polled(TwitchDevicePollResult::Granted(value)),
                    ) => TwitchAuthenticationSuccess::Granted(value),
                    (
                        TwitchAuthenticationCommand::Validate { credentials },
                        Primitive::Validated(identity),
                    ) => TwitchAuthenticationSuccess::Validated {
                        credentials,
                        identity,
                    },
                    (TwitchAuthenticationCommand::Refresh { .. }, Primitive::Refreshed(value)) => {
                        TwitchAuthenticationSuccess::Refreshed(value)
                    }
                    (TwitchAuthenticationCommand::Revoke { .. }, Primitive::Revoked) => {
                        TwitchAuthenticationSuccess::Revoked
                    }
                    _ => unreachable!("primitive matches command"),
                };
                (Ok(success), None)
            }
        };
        TwitchAuthenticationResult {
            request_id: request.id,
            operation,
            outcome,
            retained,
        }
    }
    fn failed(&self, request: &Captured) -> Self::Output {
        TwitchAuthenticationResult {
            request_id: request.id,
            operation: request.operation,
            outcome: Err(TwitchError::failed(
                request.operation,
                TwitchErrorCode::Internal,
            )),
            retained: None,
        }
    }
}
fn cancelled(operation: TwitchOperation) -> TwitchError {
    TwitchError {
        disposition: TwitchDisposition::Cancelled,
        ..TwitchError::failed(operation, TwitchErrorCode::Cancelled)
    }
}

pub struct TwitchAuthenticationWorker {
    pool: WorkerPool<Authenticate>,
}
impl TwitchAuthenticationWorker {
    pub fn new(client: Arc<dyn TwitchApi>, capacity: usize) -> Result<Self, WorkerError> {
        Ok(Self {
            pool: WorkerPool::new(
                Authenticate(client),
                WorkerOptions {
                    queue_capacity: capacity,
                    parallelism: 1,
                },
            )?,
        })
    }
    pub fn snapshot(&self) -> WorkerSnapshot {
        self.pool.snapshot()
    }
    pub fn wait_for_result(&mut self, timeout: Duration) -> Option<TwitchAuthenticationResult> {
        self.pool.wait_for_result(timeout)
    }
}
impl TwitchAuthenticator for TwitchAuthenticationWorker {
    fn enqueue(
        &mut self,
        request: TwitchAuthenticationRequest,
    ) -> Result<(), TwitchAuthenticationDispatchError> {
        if request.request_id == 0 {
            return Err(TwitchAuthenticationDispatchError::InvalidRequest);
        }
        self.pool
            .enqueue(Captured {
                id: request.request_id,
                operation: request.command.operation(),
                command: Some(request.command),
            })
            .map_err(|error| match error {
                QueueError::Full => TwitchAuthenticationDispatchError::QueueFull,
                QueueError::Stopping => TwitchAuthenticationDispatchError::Stopping,
            })
    }
    fn try_take_result(&mut self) -> Option<TwitchAuthenticationResult> {
        self.pool.try_take_result()
    }
    fn cancel_pending(&mut self) {
        self.pool.cancel_pending();
    }
}
