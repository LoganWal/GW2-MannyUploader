use manny_uploader::{
    application::*,
    domain::TwitchDeliveryReceipt,
    providers::{TwitchAuthenticationWorker, WorkerError},
};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc,
    },
    time::{Duration, SystemTime},
};

const WAIT: Duration = Duration::from_secs(5);
type Hook = dyn Fn(TwitchOperation, Option<&SecretValue>, &StopToken) -> Result<(), TwitchError>
    + Send
    + Sync;
struct Api {
    hook: Box<Hook>,
    grant: bool,
}
impl TwitchApi for Api {
    fn start_device_authorization(
        &self,
        stop: &StopToken,
    ) -> Result<TwitchDeviceAuthorization, TwitchError> {
        (self.hook)(TwitchOperation::Start, None, stop)?;
        Ok(TwitchDeviceAuthorization {
            device_code: secret(b"NEW-DEVICE"),
            user_code: "CODE".into(),
            verification_uri: "https://www.twitch.tv/activate".into(),
            expires_in: Duration::from_secs(100),
            polling_interval: Duration::from_secs(5),
        })
    }
    fn poll_device_authorization(
        &self,
        code: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchDevicePollResult, TwitchError> {
        (self.hook)(TwitchOperation::Poll, Some(code), stop)?;
        Ok(if self.grant {
            TwitchDevicePollResult::Granted(grant())
        } else {
            TwitchDevicePollResult::Pending
        })
    }
    fn validate_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchValidatedIdentity, TwitchError> {
        (self.hook)(TwitchOperation::Validate, Some(token), stop)?;
        Ok(TwitchValidatedIdentity {
            user_id: "123".into(),
            login: "synthetic_user".into(),
            expires_in: Duration::from_secs(60),
            scopes: vec![TWITCH_CHAT_SCOPE.into()],
        })
    }
    fn refresh_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchTokenGrant, TwitchError> {
        (self.hook)(TwitchOperation::Refresh, Some(token), stop)?;
        Ok(grant())
    }
    fn revoke_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<(), TwitchError> {
        (self.hook)(TwitchOperation::Revoke, Some(token), stop)
    }
    fn send_chat_message(
        &self,
        _: &str,
        _: &str,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchDeliveryReceipt, TwitchError> {
        panic!("authentication must not send chat");
    }
}
fn api(
    hook: impl Fn(TwitchOperation, Option<&SecretValue>, &StopToken) -> Result<(), TwitchError>
    + Send
    + Sync
    + 'static,
    grant: bool,
) -> Arc<dyn TwitchApi> {
    Arc::new(Api {
        hook: Box::new(hook),
        grant,
    })
}
fn secret(value: &[u8]) -> SecretValue {
    SecretValue::from_bytes(value.to_vec())
}
fn credentials() -> TwitchCredentialSet {
    TwitchCredentialSet {
        access_token: secret(b"OLD-ACCESS"),
        refresh_token: secret(b"OLD-REFRESH"),
        access_expires_at: SystemTime::UNIX_EPOCH + Duration::from_secs(1_800_000_000),
    }
}
fn grant() -> TwitchTokenGrant {
    TwitchTokenGrant {
        access_token: secret(b"NEW-ACCESS"),
        refresh_token: secret(b"NEW-REFRESH"),
        expires_in: Duration::from_secs(60),
        scopes: vec![TWITCH_CHAT_SCOPE.into()],
    }
}
const OPERATIONS: [TwitchOperation; 5] = [
    TwitchOperation::Start,
    TwitchOperation::Poll,
    TwitchOperation::Validate,
    TwitchOperation::Refresh,
    TwitchOperation::Revoke,
];
fn request(id: u64, op: TwitchOperation) -> TwitchAuthenticationRequest {
    TwitchAuthenticationRequest {
        request_id: id,
        command: match op {
            TwitchOperation::Start => TwitchAuthenticationCommand::Start,
            TwitchOperation::Poll => TwitchAuthenticationCommand::Poll {
                device_code: secret(b"OLD-DEVICE"),
            },
            TwitchOperation::Validate => TwitchAuthenticationCommand::Validate {
                credentials: credentials(),
            },
            TwitchOperation::Refresh => TwitchAuthenticationCommand::Refresh {
                credentials: credentials(),
            },
            TwitchOperation::Revoke => TwitchAuthenticationCommand::Revoke {
                access_token: secret(b"OLD-ACCESS"),
            },
            TwitchOperation::SendChat => unreachable!(),
        },
    }
}
fn addresses(command: &TwitchAuthenticationCommand) -> (usize, usize) {
    match command {
        TwitchAuthenticationCommand::Poll { device_code } => {
            (device_code.expose().as_ptr() as usize, 0)
        }
        TwitchAuthenticationCommand::Validate { credentials }
        | TwitchAuthenticationCommand::Refresh { credentials } => (
            credentials.access_token.expose().as_ptr() as usize,
            credentials.refresh_token.expose().as_ptr() as usize,
        ),
        TwitchAuthenticationCommand::Revoke { access_token } => {
            (access_token.expose().as_ptr() as usize, 0)
        }
        _ => (0, 0),
    }
}

#[test]
fn every_command_maps_once_to_its_primitive_and_returns_correlated_success() {
    for op in OPERATIONS {
        let command = request(42, op);
        let original = addresses(&command.command);
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let mut worker = TwitchAuthenticationWorker::new(
            api(
                move |actual, token, _| {
                    count.fetch_add(1, Ordering::Relaxed);
                    assert_eq!(actual, op);
                    if let Some(token) = token {
                        assert_eq!(
                            token.expose().as_ptr() as usize,
                            if op == TwitchOperation::Refresh {
                                original.1
                            } else {
                                original.0
                            }
                        );
                    } else {
                        assert_eq!(op, TwitchOperation::Start);
                    }
                    Ok(())
                },
                false,
            ),
            2,
        )
        .unwrap();
        worker.enqueue(command).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(result.request_id, 42);
        assert_eq!(result.operation, op);
        assert!(result.retained.is_none());
        let debug = format!("{result:?}");
        for private in [
            "OLD-ACCESS",
            "OLD-REFRESH",
            "OLD-DEVICE",
            "NEW-DEVICE",
            "synthetic_user",
        ] {
            assert!(!debug.contains(private));
        }
        match result.outcome.unwrap() {
            TwitchAuthenticationSuccess::Started(value) => {
                assert_eq!(op, TwitchOperation::Start);
                assert_eq!(value.device_code.expose(), b"NEW-DEVICE");
                assert_eq!(value.polling_interval, Duration::from_secs(5));
            }
            TwitchAuthenticationSuccess::Pending { device_code } => {
                assert_eq!(op, TwitchOperation::Poll);
                assert_eq!(device_code.expose().as_ptr() as usize, original.0);
            }
            TwitchAuthenticationSuccess::Validated {
                credentials,
                identity,
            } => {
                assert_eq!(op, TwitchOperation::Validate);
                assert_eq!(
                    credentials.access_token.expose().as_ptr() as usize,
                    original.0
                );
                assert_eq!(
                    credentials.refresh_token.expose().as_ptr() as usize,
                    original.1
                );
                assert_eq!(
                    credentials.access_expires_at,
                    SystemTime::UNIX_EPOCH + Duration::from_secs(1_800_000_000)
                );
                assert_eq!(identity.user_id, "123");
            }
            TwitchAuthenticationSuccess::Refreshed(value) => {
                assert_eq!(op, TwitchOperation::Refresh);
                assert_eq!(value.access_token.expose(), b"NEW-ACCESS");
                assert_eq!(value.refresh_token.expose(), b"NEW-REFRESH");
            }
            TwitchAuthenticationSuccess::Revoked => assert_eq!(op, TwitchOperation::Revoke),
            _ => panic!("unexpected grant"),
        }
        assert_eq!(calls.load(Ordering::Relaxed), 1);
        assert!(worker.try_take_result().is_none());
    }
}

#[test]
fn granted_poll_moves_replacement_pair_without_retaining_the_device_code() {
    let mut worker = TwitchAuthenticationWorker::new(api(|_, _, _| Ok(()), true), 1).unwrap();
    worker.enqueue(request(1, TwitchOperation::Poll)).unwrap();
    let result = worker.wait_for_result(WAIT).unwrap();
    assert!(result.retained.is_none());
    let TwitchAuthenticationSuccess::Granted(grant) = result.outcome.unwrap() else {
        panic!("expected grant");
    };
    assert_eq!(grant.access_token.expose(), b"NEW-ACCESS");
    assert_eq!(grant.refresh_token.expose(), b"NEW-REFRESH");
    assert_eq!(grant.scopes, [TWITCH_CHAT_SCOPE]);
}

#[test]
fn failures_return_the_unmodified_command_secret_without_copying_retrying_or_chaining() {
    for op in OPERATIONS {
        for disposition in [
            TwitchDisposition::Retry,
            TwitchDisposition::Reconnect,
            TwitchDisposition::Failed,
            TwitchDisposition::Cancelled,
        ] {
            let command = request(7, op);
            let original = addresses(&command.command);
            let calls = Arc::new(AtomicUsize::new(0));
            let count = calls.clone();
            let mut worker = TwitchAuthenticationWorker::new(
                api(
                    move |_, _, _| {
                        count.fetch_add(1, Ordering::Relaxed);
                        Err(TwitchError {
                            disposition,
                            retry_after: Some(Duration::from_secs(7)),
                            ..TwitchError::failed(
                                TwitchOperation::Start,
                                TwitchErrorCode::HttpStatus,
                            )
                        })
                    },
                    false,
                ),
                1,
            )
            .unwrap();
            worker.enqueue(command).unwrap();
            let result = worker.wait_for_result(WAIT).unwrap();
            assert_eq!(result.request_id, 7);
            assert_eq!(result.operation, op);
            let error = result.outcome.unwrap_err();
            assert_eq!(error.operation, op);
            assert_eq!(error.disposition, disposition);
            assert_eq!(error.retry_after, Some(Duration::from_secs(7)));
            match result.retained {
                Some(TwitchAuthenticationSecret::DeviceCode(code)) => {
                    assert_eq!(op, TwitchOperation::Poll);
                    assert_eq!(code.expose().as_ptr() as usize, original.0);
                }
                Some(TwitchAuthenticationSecret::Credentials(value)) => {
                    assert!(matches!(
                        op,
                        TwitchOperation::Validate | TwitchOperation::Refresh
                    ));
                    assert_eq!(value.access_token.expose().as_ptr() as usize, original.0);
                    assert_eq!(value.refresh_token.expose().as_ptr() as usize, original.1);
                    assert_eq!(value.access_expires_at, credentials().access_expires_at);
                }
                None => assert!(matches!(
                    op,
                    TwitchOperation::Start | TwitchOperation::Revoke
                )),
            }
            assert_eq!(calls.load(Ordering::Relaxed), 1);
            assert!(worker.try_take_result().is_none());
        }
    }
}

#[test]
fn client_panics_retain_recoverable_ownership_and_return_only_typed_failure() {
    for op in OPERATIONS {
        let mut worker = TwitchAuthenticationWorker::new(
            api(|_, _, _| panic!("synthetic client failure"), false),
            1,
        )
        .unwrap();
        let request = request(1, op);
        let original = addresses(&request.command);
        worker.enqueue(request).unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert!(!format!("{result:?}").contains("synthetic"));
        assert_eq!(result.outcome.unwrap_err().code, TwitchErrorCode::Internal);
        match result.retained {
            Some(TwitchAuthenticationSecret::DeviceCode(code)) => {
                assert_eq!(code.expose().as_ptr() as usize, original.0)
            }
            Some(TwitchAuthenticationSecret::Credentials(value)) => {
                assert_eq!(value.refresh_token.expose().as_ptr() as usize, original.1)
            }
            None => assert!(matches!(
                op,
                TwitchOperation::Start | TwitchOperation::Revoke
            )),
        }
    }
}

#[test]
fn capacity_and_request_ids_are_checked_and_shutdown_rejects_admission() {
    let api = api(|_, _, _| panic!("unexpected call"), false);
    assert!(matches!(
        TwitchAuthenticationWorker::new(api.clone(), 0),
        Err(WorkerError::InvalidCapacity)
    ));
    let mut worker = TwitchAuthenticationWorker::new(api, 1).unwrap();
    assert_eq!(
        worker.enqueue(request(0, TwitchOperation::Start)),
        Err(TwitchAuthenticationDispatchError::InvalidRequest)
    );
    assert!(worker.try_take_result().is_none());
    worker.cancel_pending();
    assert_eq!(
        worker.enqueue(request(1, TwitchOperation::Start)),
        Err(TwitchAuthenticationDispatchError::Stopping)
    );
    assert!(worker.snapshot().stopping);
}

#[test]
fn one_thread_and_bounded_queues_preserve_fifo_during_result_backpressure() {
    let (entered, ready) = mpsc::sync_channel(4);
    let (release, wait) = mpsc::sync_channel(4);
    let wait = Mutex::new(wait);
    let mut worker = TwitchAuthenticationWorker::new(
        api(
            move |op, _, _| {
                entered.send(op).unwrap();
                wait.lock().unwrap().recv_timeout(WAIT).unwrap();
                Ok(())
            },
            false,
        ),
        1,
    )
    .unwrap();
    worker.enqueue(request(1, TwitchOperation::Start)).unwrap();
    assert_eq!(ready.recv_timeout(WAIT).unwrap(), TwitchOperation::Start);
    worker.enqueue(request(2, TwitchOperation::Poll)).unwrap();
    assert_eq!(
        worker.enqueue(request(3, TwitchOperation::Validate)),
        Err(TwitchAuthenticationDispatchError::QueueFull)
    );
    release.send(()).unwrap();
    assert_eq!(ready.recv_timeout(WAIT).unwrap(), TwitchOperation::Poll);
    worker
        .enqueue(request(3, TwitchOperation::Validate))
        .unwrap();
    release.send(()).unwrap();
    assert_eq!(worker.snapshot().owned_threads, 1);
    assert_eq!(worker.snapshot().parallelism, 1);
    assert_eq!(worker.wait_for_result(WAIT).unwrap().request_id, 1);
    assert_eq!(worker.wait_for_result(WAIT).unwrap().request_id, 2);
    assert_eq!(ready.recv_timeout(WAIT).unwrap(), TwitchOperation::Validate);
    release.send(()).unwrap();
    assert_eq!(worker.wait_for_result(WAIT).unwrap().request_id, 3);
}

#[test]
fn cancellation_discards_pending_secrets_and_drop_joins_the_active_primitive() {
    let exited = Arc::new(AtomicBool::new(false));
    let observed = exited.clone();
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let (entered, ready) = mpsc::sync_channel(1);
    let mut worker = TwitchAuthenticationWorker::new(
        api(
            move |_, _, stop| {
                count.fetch_add(1, Ordering::Relaxed);
                entered.send(()).unwrap();
                assert!(stop.wait_for_stop(WAIT));
                observed.store(true, Ordering::Release);
                Ok(())
            },
            false,
        ),
        2,
    )
    .unwrap();
    worker
        .enqueue(request(1, TwitchOperation::Refresh))
        .unwrap();
    ready.recv_timeout(WAIT).unwrap();
    worker
        .enqueue(request(2, TwitchOperation::Validate))
        .unwrap();
    worker.cancel_pending();
    worker.cancel_pending();
    assert!(worker.try_take_result().is_none());
    drop(worker);
    assert!(exited.load(Ordering::Acquire));
    assert_eq!(calls.load(Ordering::Relaxed), 1);
}
