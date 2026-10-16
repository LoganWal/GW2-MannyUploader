use manny_uploader::{application::*, domain::*, providers::*};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    time::{Duration, SystemTime},
};
const WAIT: Duration = Duration::from_secs(5);
type SendHook = dyn Fn(&str, &str, &SecretValue, &StopToken) -> Result<TwitchDeliveryReceipt, TwitchError>
    + Send
    + Sync;
struct Api(Box<SendHook>);
impl TwitchApi for Api {
    fn start_device_authorization(
        &self,
        _: &StopToken,
    ) -> Result<TwitchDeviceAuthorization, TwitchError> {
        panic!("chat must not start authorization")
    }
    fn poll_device_authorization(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchDevicePollResult, TwitchError> {
        panic!("chat must not poll authorization")
    }
    fn validate_access_token(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchValidatedIdentity, TwitchError> {
        panic!("chat must use session access")
    }
    fn refresh_access_token(
        &self,
        _: &SecretValue,
        _: &StopToken,
    ) -> Result<TwitchTokenGrant, TwitchError> {
        panic!("chat must use session access")
    }
    fn revoke_access_token(&self, _: &SecretValue, _: &StopToken) -> Result<(), TwitchError> {
        panic!("chat must not revoke")
    }
    fn send_chat_message(
        &self,
        user: &str,
        message: &str,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchDeliveryReceipt, TwitchError> {
        (self.0)(user, message, token, stop)
    }
}
type AcquireHook =
    dyn Fn(&StopToken) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> + Send + Sync;
type RecoverHook = dyn Fn(
        TwitchDeliverySession,
        &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError>
    + Send
    + Sync;
struct Sessions {
    acquire: Box<AcquireHook>,
    recover: Box<RecoverHook>,
}
impl TwitchDeliverySessionAccess for Sessions {
    fn acquire(
        &self,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        (self.acquire)(stop)
    }
    fn recover(
        &self,
        rejected: TwitchDeliverySession,
        stop: &StopToken,
    ) -> Result<TwitchDeliverySession, TwitchDeliverySessionError> {
        (self.recover)(rejected, stop)
    }
}
fn lease(revision: u64) -> TwitchDeliverySession {
    TwitchDeliverySession {
        access_token: SecretValue::from_bytes(format!("synthetic-token-{revision}").into_bytes()),
        authenticated_user_id: "123".into(),
        revision,
    }
}
fn sessions() -> Arc<dyn TwitchDeliverySessionAccess> {
    Arc::new(Sessions {
        acquire: Box::new(|_| Ok(lease(1))),
        recover: Box::new(|rejected, _| {
            assert_eq!(rejected.revision, 1);
            assert_eq!(rejected.authenticated_user_id, "123");
            assert_eq!(rejected.access_token.expose(), b"synthetic-token-1");
            Ok(lease(2))
        }),
    })
}
fn client(
    f: impl Fn(&str, &str, &SecretValue, &StopToken) -> Result<TwitchDeliveryReceipt, TwitchError>
    + Send
    + Sync
    + 'static,
) -> Arc<dyn TwitchApi> {
    Arc::new(Api(Box::new(f)))
}
fn sent() -> TwitchDeliveryReceipt {
    TwitchDeliveryReceipt {
        status: TwitchDeliveryStatus::Sent,
        message_id: Some("synthetic-message-id".into()),
    }
}
fn status(code: u16) -> TwitchError {
    TwitchError {
        http_status: Some(code),
        disposition: if code == 401 {
            TwitchDisposition::Reconnect
        } else {
            TwitchDisposition::Retry
        },
        retry_after: Some(Duration::from_secs(17)),
        ..TwitchError::failed(TwitchOperation::SendChat, TwitchErrorCode::HttpStatus)
    }
}
fn transport(code: HttpErrorCode) -> TwitchError {
    TwitchError {
        http_error: Some(code),
        disposition: TwitchDisposition::Retry,
        retry_after: Some(Duration::from_secs(17)),
        ..TwitchError::failed(TwitchOperation::SendChat, TwitchErrorCode::Transport)
    }
}
fn send(delivery: &TwitchChatDelivery) -> TwitchChatResult {
    delivery.send("synthetic message", &CancellationSource::default().token())
}
fn config() -> TwitchProviderConfig {
    TwitchProviderConfig {
        message_template: "{encounter}{mode_suffix}: {result} {boss_id} {url}".into(),
        post_success: true,
        post_failure: true,
    }
}
fn request(id: u64) -> UploadRequest {
    UploadRequest {
        job_id: UploadJobId::new(id).unwrap(),
        provider: Provider::Twitch,
        attempt: 3,
        file: LogFileIdentity {
            canonical_path: "absent-fixture.zevtc".into(),
            size: 7,
            last_write_time: SystemTime::UNIX_EPOCH,
        },
        metadata: EncounterMetadata {
            boss_id: 999,
            pov_account: "Unrelated.1234".into(),
            remaining_health_basis_points: None,
        },
        dps_report_result: Some(DpsReportResult {
            permalink: "https://dps.report/fixture".into(),
            encounter_name: "Boss".into(),
            boss_id: 123,
            mode: "CM".into(),
            success: true,
        }),
        intent: UploadIntent::Automatic,
    }
}
fn worker(client: Arc<dyn TwitchApi>) -> TwitchProviderWorker {
    TwitchProviderWorker::new(
        client,
        sessions(),
        config(),
        WorkerOptions {
            queue_capacity: 2,
            parallelism: 1,
        },
    )
    .unwrap()
}
fn outcome(worker: &mut TwitchProviderWorker) -> UploadOutcome {
    let result = worker.wait_for_result(WAIT).expect("worker result");
    assert_eq!(result.provider, Provider::Twitch);
    assert_eq!(result.attempt, 3);
    result.outcome
}

#[test]
fn same_broadcaster_and_message_are_used_across_exactly_one_recovery() {
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    let delivery = TwitchChatDelivery::new(
        client(move |user, message, token, _| {
            assert_eq!(user, "123");
            assert_eq!(message, "synthetic message");
            if observed.fetch_add(1, Ordering::SeqCst) == 0 {
                assert_eq!(token.expose(), b"synthetic-token-1");
                Err(status(401))
            } else {
                assert_eq!(token.expose(), b"synthetic-token-2");
                Ok(sent())
            }
        }),
        sessions(),
    );
    assert_eq!(send(&delivery), TwitchChatResult::Sent(sent()));
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}

#[test]
fn second_401_fails_without_another_recovery() {
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    let delivery = TwitchChatDelivery::new(
        client(move |_, _, _, _| {
            observed.fetch_add(1, Ordering::SeqCst);
            Err(status(401))
        }),
        sessions(),
    );
    assert_eq!(
        send(&delivery),
        TwitchChatResult::Failed(TwitchChatError::ReconnectRequired)
    );
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}

#[test]
fn recovery_cannot_change_broadcaster_or_return_an_older_revision() {
    for foreign in [false, true] {
        let calls = Arc::new(AtomicUsize::new(0));
        let observed = calls.clone();
        let access = Arc::new(Sessions {
            acquire: Box::new(|_| Ok(lease(5))),
            recover: Box::new(move |_, _| {
                let mut replacement = lease(if foreign { 6 } else { 4 });
                if foreign {
                    replacement.authenticated_user_id = "456".into();
                }
                Ok(replacement)
            }),
        });
        let delivery = TwitchChatDelivery::new(
            client(move |_, _, _, _| {
                observed.fetch_add(1, Ordering::SeqCst);
                Err(status(401))
            }),
            access,
        );
        assert_eq!(
            send(&delivery),
            TwitchChatResult::Failed(TwitchChatError::ReconnectRequired)
        );
        assert_eq!(calls.load(Ordering::SeqCst), 1);
    }
}

#[test]
fn session_failures_have_safe_diagnostics_and_bounded_retry_delays() {
    for recovering in [false, true] {
        for code in [
            TwitchDeliverySessionErrorCode::NotConnected,
            TwitchDeliverySessionErrorCode::Retry,
            TwitchDeliverySessionErrorCode::ReconnectRequired,
            TwitchDeliverySessionErrorCode::Failed,
            TwitchDeliverySessionErrorCode::Cancelled,
        ] {
            for delay in [0, 17, 86400, 86401] {
                let error = TwitchDeliverySessionError {
                    code,
                    retry_after: Some(Duration::from_secs(delay)),
                };
                let access = Arc::new(Sessions {
                    acquire: Box::new(move |_| if recovering { Ok(lease(1)) } else { Err(error) }),
                    recover: Box::new(move |_, _| Err(error)),
                });
                let delivery = TwitchChatDelivery::new(
                    client(move |_, _, _, _| {
                        assert!(recovering);
                        Err(status(401))
                    }),
                    access,
                );
                let expected = match code {
                    TwitchDeliverySessionErrorCode::NotConnected => {
                        TwitchChatResult::Failed(TwitchChatError::NotConnected)
                    }
                    TwitchDeliverySessionErrorCode::Retry => TwitchChatResult::RetryAfter(
                        Duration::from_secs(if (1..=86400).contains(&delay) {
                            delay
                        } else {
                            30
                        }),
                    ),
                    TwitchDeliverySessionErrorCode::ReconnectRequired => {
                        TwitchChatResult::Failed(TwitchChatError::ReconnectRequired)
                    }
                    TwitchDeliverySessionErrorCode::Failed => {
                        TwitchChatResult::Failed(TwitchChatError::SessionFailed)
                    }
                    TwitchDeliverySessionErrorCode::Cancelled => TwitchChatResult::Cancelled,
                };
                assert_eq!(send(&delivery), expected);
            }
        }
    }
}

#[test]
fn transport_retries_are_limited_to_connection_setup() {
    for code in [
        HttpErrorCode::InvalidRequest,
        HttpErrorCode::BodyReadFailed,
        HttpErrorCode::Cancelled,
        HttpErrorCode::Timeout,
        HttpErrorCode::NameResolutionFailed,
        HttpErrorCode::ConnectionFailed,
        HttpErrorCode::TlsFailed,
        HttpErrorCode::SendFailed,
        HttpErrorCode::ReceiveFailed,
        HttpErrorCode::ResponseTooLarge,
        HttpErrorCode::ProtocolError,
        HttpErrorCode::UnsupportedEnvironment,
        HttpErrorCode::InitializationFailed,
        HttpErrorCode::Internal,
    ] {
        let delivery =
            TwitchChatDelivery::new(client(move |_, _, _, _| Err(transport(code))), sessions());
        assert_eq!(
            send(&delivery),
            if matches!(
                code,
                HttpErrorCode::NameResolutionFailed
                    | HttpErrorCode::ConnectionFailed
                    | HttpErrorCode::TlsFailed
            ) {
                TwitchChatResult::RetryAfter(Duration::from_secs(17))
            } else {
                TwitchChatResult::Ambiguous
            },
            "{code:?}"
        );
    }
}

#[test]
fn only_rate_limit_status_retries_and_408_is_ambiguous() {
    for code in [200, 302, 400, 403, 404, 408, 409, 429, 500, 503] {
        let delivery =
            TwitchChatDelivery::new(client(move |_, _, _, _| Err(status(code))), sessions());
        let expected = match code {
            429 => TwitchChatResult::RetryAfter(Duration::from_secs(17)),
            400 | 403 | 404 | 409 => TwitchChatResult::Failed(TwitchChatError::SendFailed),
            _ => TwitchChatResult::Ambiguous,
        };
        assert_eq!(send(&delivery), expected, "{code}");
    }
    for seconds in [0, 86400, 86401] {
        let delivery = TwitchChatDelivery::new(
            client(move |_, _, _, _| {
                let mut error = status(429);
                error.retry_after = Some(Duration::from_secs(seconds));
                Err(error)
            }),
            sessions(),
        );
        assert_eq!(
            send(&delivery),
            TwitchChatResult::RetryAfter(Duration::from_secs(if seconds == 86400 {
                seconds
            } else {
                30
            }))
        );
    }
}

#[test]
fn malformed_success_is_ambiguous_and_normalized_drops_preserve_only_receipts() {
    for receipt in [
        TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::Sent,
            message_id: None,
        },
        TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::Sent,
            message_id: Some("line\nbreak".into()),
        },
        TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::AutoMod,
            message_id: Some("unexpected".into()),
        },
    ] {
        let delivery =
            TwitchChatDelivery::new(client(move |_, _, _, _| Ok(receipt.clone())), sessions());
        assert_eq!(send(&delivery), TwitchChatResult::Ambiguous);
    }
    for status in [
        TwitchDeliveryStatus::AutoMod,
        TwitchDeliveryStatus::BlockedTerm,
        TwitchDeliveryStatus::Duplicate,
        TwitchDeliveryStatus::RateLimited,
        TwitchDeliveryStatus::FollowersOnly,
        TwitchDeliveryStatus::SlowMode,
        TwitchDeliveryStatus::SubscribersOnly,
        TwitchDeliveryStatus::Restricted,
        TwitchDeliveryStatus::OtherDrop,
    ] {
        let receipt = TwitchDeliveryReceipt {
            status,
            message_id: None,
        };
        let response = receipt.clone();
        let delivery =
            TwitchChatDelivery::new(client(move |_, _, _, _| Ok(response.clone())), sessions());
        assert_eq!(send(&delivery), TwitchChatResult::Dropped(receipt));
        assert!(!twitch_drop_detail(status).is_empty());
    }
}

#[test]
fn cancellation_before_sending_is_safe_but_after_client_entry_is_ambiguous() {
    for stage in 0..4 {
        let cancel = Arc::new(CancellationSource::default());
        if stage == 0 {
            cancel.request_stop();
        }
        let access_cancel = cancel.clone();
        let recovery_cancel = cancel.clone();
        let send_cancel = cancel.clone();
        let access = Arc::new(Sessions {
            acquire: Box::new(move |_| {
                assert_ne!(stage, 0);
                if stage == 1 {
                    access_cancel.request_stop();
                }
                Ok(lease(1))
            }),
            recover: Box::new(move |_, _| {
                assert_eq!(stage, 3);
                recovery_cancel.request_stop();
                Ok(lease(2))
            }),
        });
        let delivery = TwitchChatDelivery::new(
            client(move |_, _, _, _| {
                assert!(stage >= 2);
                if stage == 2 {
                    send_cancel.request_stop();
                    Ok(sent())
                } else {
                    Err(status(401))
                }
            }),
            access,
        );
        assert_eq!(
            delivery.send("message", &cancel.token()),
            if stage == 2 {
                TwitchChatResult::Ambiguous
            } else {
                TwitchChatResult::Cancelled
            }
        );
    }
}

#[test]
fn panics_before_sending_fail_safely_and_client_panics_are_ambiguous() {
    for stage in 0..3 {
        let access = Arc::new(Sessions {
            acquire: Box::new(move |_| {
                assert_ne!(stage, 0, "synthetic acquire panic");
                Ok(lease(1))
            }),
            recover: Box::new(|_, _| panic!("synthetic recovery panic")),
        });
        let delivery = TwitchChatDelivery::new(
            client(move |_, _, _, _| {
                assert_ne!(stage, 2, "synthetic send panic");
                Err(status(401))
            }),
            access,
        );
        assert_eq!(
            send(&delivery),
            if stage == 2 {
                TwitchChatResult::Ambiguous
            } else {
                TwitchChatResult::Failed(TwitchChatError::SessionFailed)
            }
        );
    }
}

#[test]
fn invalid_messages_and_leases_never_enter_the_client() {
    let no_send = client(|_, _, _, _| panic!("must not send"));
    for message in [
        String::new(),
        "a".repeat(501),
        "x\ny".into(),
        "\u{1f600}".repeat(501),
    ] {
        let delivery = TwitchChatDelivery::new(no_send.clone(), sessions());
        assert_eq!(
            delivery.send(&message, &CancellationSource::default().token()),
            TwitchChatResult::Failed(TwitchChatError::InvalidMessage)
        );
    }
    for case in 0..3 {
        let access = Arc::new(Sessions {
            acquire: Box::new(move |_| {
                let mut value = lease(1);
                match case {
                    0 => value.revision = 0,
                    1 => value.authenticated_user_id = "001".into(),
                    _ => value.access_token = SecretValue::default(),
                };
                Ok(value)
            }),
            recover: Box::new(|_, _| panic!("must not recover")),
        });
        assert_eq!(
            send(&TwitchChatDelivery::new(no_send.clone(), access)),
            TwitchChatResult::Failed(TwitchChatError::SessionFailed)
        );
    }
    let delivery = TwitchChatDelivery::new(
        client(|_, message, _, _| {
            assert_eq!(message.len(), 2000);
            Ok(sent())
        }),
        sessions(),
    );
    assert_eq!(
        delivery.send(
            &"\u{1f600}".repeat(500),
            &CancellationSource::default().token()
        ),
        TwitchChatResult::Sent(sent())
    );
}

#[test]
fn worker_renders_only_same_job_report_fields_and_correlates_results() {
    let mut worker = worker(client(|user, message, token, _| {
        assert_eq!(user, "123");
        assert_eq!(token.expose(), b"synthetic-token-1");
        assert_eq!(message, "Boss (CM): Success 123 https://dps.report/fixture");
        Ok(sent())
    }));
    worker.enqueue(request(42)).unwrap();
    let result = worker.wait_for_result(WAIT).unwrap();
    assert_eq!(
        (result.job_id.get(), result.provider, result.attempt),
        (42, Provider::Twitch, 3)
    );
    assert_eq!(
        result.outcome,
        UploadOutcome::Succeeded(UploadSuccess::Twitch(sent()))
    );
}

#[test]
fn policy_skips_do_not_acquire_credentials_or_touch_the_ledger() {
    let access = Arc::new(Sessions {
        acquire: Box::new(|_| panic!("excluded request acquired credentials")),
        recover: Box::new(|_, _| panic!()),
    });
    let mut worker = TwitchProviderWorker::new(
        client(|_, _, _, _| panic!()),
        access,
        TwitchProviderConfig {
            post_success: false,
            post_failure: false,
            ..config()
        },
        WorkerOptions::default(),
    )
    .unwrap();
    for success in [false, true] {
        let mut request = request(1);
        request.dps_report_result.as_mut().unwrap().success = success;
        worker.enqueue(request).unwrap();
        assert_eq!(outcome(&mut worker), UploadOutcome::Skipped);
    }
}

#[test]
fn settled_sent_and_ambiguous_keys_deduplicate_until_an_explicit_user_action() {
    for ambiguous in [false, true] {
        let calls = Arc::new(AtomicUsize::new(0));
        let observed = calls.clone();
        let mut worker = worker(client(move |_, _, _, _| {
            observed.fetch_add(1, Ordering::SeqCst);
            if ambiguous {
                Err(transport(HttpErrorCode::Timeout))
            } else {
                Ok(sent())
            }
        }));
        let expected = if ambiguous {
            UploadOutcome::Ambiguous
        } else {
            UploadOutcome::Succeeded(UploadSuccess::Twitch(sent()))
        };
        for _ in 0..3 {
            worker.enqueue(request(1)).unwrap();
            assert_eq!(outcome(&mut worker), expected);
        }
        assert_eq!(calls.load(Ordering::SeqCst), 1);
        for intent in [
            UploadIntent::ManualRetry,
            UploadIntent::Reupload,
            UploadIntent::Rechat,
        ] {
            let mut request = request(1);
            request.intent = intent;
            worker.enqueue(request).unwrap();
            assert_eq!(outcome(&mut worker), expected);
        }
        assert_eq!(calls.load(Ordering::SeqCst), 4);
    }
}

#[test]
fn ledger_keys_include_job_id_and_permalink_and_evict_after_256_entries() {
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    let mut worker = worker(client(move |_, _, _, _| {
        observed.fetch_add(1, Ordering::SeqCst);
        Ok(sent())
    }));
    for id in 1..=256 {
        worker.enqueue(request(id)).unwrap();
        outcome(&mut worker);
    }
    worker.enqueue(request(1)).unwrap();
    outcome(&mut worker);
    assert_eq!(calls.load(Ordering::SeqCst), 256);
    let mut different = request(1);
    different.dps_report_result.as_mut().unwrap().permalink = "https://dps.report/another".into();
    worker.enqueue(different).unwrap();
    outcome(&mut worker);
    worker.enqueue(request(1)).unwrap();
    outcome(&mut worker);
    assert_eq!(calls.load(Ordering::SeqCst), 258);
}

#[test]
fn configuration_is_captured_before_queued_work_starts() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let release_rx = Mutex::new(release_rx);
    let messages = Arc::new(Mutex::new(Vec::new()));
    let observed = messages.clone();
    let mut worker = worker(client(move |_, message, _, _| {
        let first = {
            let mut values = observed.lock().unwrap();
            values.push(message.to_owned());
            values.len() == 1
        };
        if first {
            entered_tx.send(()).unwrap();
            release_rx.lock().unwrap().recv_timeout(WAIT).unwrap();
        }
        Ok(sent())
    }));
    worker.enqueue(request(1)).unwrap();
    entered_rx.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(2)).unwrap();
    worker
        .update_config(TwitchProviderConfig {
            message_template: "changed {url}".into(),
            ..config()
        })
        .unwrap();
    worker.enqueue(request(3)).unwrap();
    release_tx.send(()).unwrap();
    for _ in 0..3 {
        assert!(matches!(outcome(&mut worker), UploadOutcome::Succeeded(_)));
    }
    let values = messages.lock().unwrap();
    assert_eq!(values[0], values[1]);
    assert_eq!(values[2], "changed https://dps.report/fixture");
}

#[test]
fn concurrent_same_key_attempts_do_not_send_twice() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let release_rx = Mutex::new(release_rx);
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    let mut worker = worker(client(move |_, _, _, _| {
        observed.fetch_add(1, Ordering::SeqCst);
        entered_tx.send(()).unwrap();
        release_rx.lock().unwrap().recv_timeout(WAIT).unwrap();
        Ok(sent())
    }));
    worker.update_parallelism(2).unwrap();
    worker.enqueue(request(1)).unwrap();
    entered_rx.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(1)).unwrap();
    assert_eq!(
        outcome(&mut worker),
        UploadOutcome::RetryAfter(Duration::from_secs(30))
    );
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    release_tx.send(()).unwrap();
    assert_eq!(
        outcome(&mut worker),
        UploadOutcome::Succeeded(UploadSuccess::Twitch(sent()))
    );
    worker.enqueue(request(1)).unwrap();
    assert_eq!(
        outcome(&mut worker),
        UploadOutcome::Succeeded(UploadSuccess::Twitch(sent()))
    );
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn admission_and_configuration_validation_leave_prior_options_intact() {
    let mut worker = worker(client(|_, _, _, _| panic!("invalid request sent")));
    for case in 0..5 {
        let mut request = request(1);
        match case {
            0 => request.provider = Provider::DonBot,
            1 => request.attempt = 0,
            2 => request.file.canonical_path.clear(),
            3 => request.dps_report_result = None,
            _ => {
                request.dps_report_result.as_mut().unwrap().permalink =
                    "https://evil.invalid/path?key=secret".into()
            }
        };
        assert_eq!(worker.enqueue(request), Err(ProviderError::InvalidRequest));
    }
    let invalid = TwitchProviderConfig {
        message_template: "no URL".into(),
        ..config()
    };
    assert_eq!(
        worker.update_config(invalid),
        Err(TwitchProviderWorkerError::InvalidConfiguration)
    );
    assert_eq!(worker.config_snapshot(), config());
    worker.cancel_pending();
    assert_eq!(worker.enqueue(request(1)), Err(ProviderError::Stopping));
}

#[test]
fn destruction_cancels_and_joins_active_chat_and_discards_queued_work() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let finished = Arc::new(AtomicUsize::new(0));
    let observed = finished.clone();
    let mut worker = worker(client(move |_, _, _, stop| {
        entered_tx.send(()).unwrap();
        assert!(stop.wait_for_stop(WAIT));
        observed.fetch_add(1, Ordering::SeqCst);
        Err(transport(HttpErrorCode::Cancelled))
    }));
    worker.enqueue(request(1)).unwrap();
    entered_rx.recv_timeout(WAIT).unwrap();
    worker.enqueue(request(2)).unwrap();
    drop(worker);
    assert_eq!(finished.load(Ordering::SeqCst), 1);
}

#[test]
fn test_worker_uses_unique_fixed_text_and_discards_remote_message_ids() {
    let texts = Arc::new(Mutex::new(Vec::new()));
    let observed = texts.clone();
    let mut worker = TwitchTestMessageWorker::new(
        client(move |user, message, token, _| {
            assert_eq!(user, "123");
            assert_eq!(token.expose(), b"synthetic-token-1");
            observed.lock().unwrap().push(message.to_owned());
            Ok(sent())
        }),
        sessions(),
        2,
    )
    .unwrap();
    for id in [1, u64::MAX] {
        worker
            .enqueue(TwitchTestMessageRequest { request_id: id })
            .unwrap();
        let result = worker.wait_for_result(WAIT).unwrap();
        assert_eq!(
            result,
            TwitchTestMessageResult {
                request_id: id,
                outcome: TwitchTestMessageOutcome::Sent
            }
        );
        assert!(!format!("{result:?}").contains("synthetic-message-id"));
    }
    assert_eq!(
        *texts.lock().unwrap(),
        [
            "GW2 Manny Uploader test message #1",
            "GW2 Manny Uploader test message #18446744073709551615"
        ]
    );
}

#[test]
fn test_worker_maps_chat_outcomes_without_retrying() {
    for (code, expected) in [
        (
            403,
            TwitchTestMessageOutcome::Failed(TwitchChatError::SendFailed),
        ),
        (
            429,
            TwitchTestMessageOutcome::RetryAfter(Duration::from_secs(17)),
        ),
        (408, TwitchTestMessageOutcome::Ambiguous),
    ] {
        let calls = Arc::new(AtomicUsize::new(0));
        let observed = calls.clone();
        let mut worker = TwitchTestMessageWorker::new(
            client(move |_, _, _, _| {
                observed.fetch_add(1, Ordering::SeqCst);
                Err(status(code))
            }),
            sessions(),
            1,
        )
        .unwrap();
        worker
            .enqueue(TwitchTestMessageRequest { request_id: 8 })
            .unwrap();
        assert_eq!(worker.wait_for_result(WAIT).unwrap().outcome, expected);
        assert_eq!(calls.load(Ordering::SeqCst), 1);
    }
    let mut worker = TwitchTestMessageWorker::new(
        client(|_, _, _, _| {
            Ok(TwitchDeliveryReceipt {
                status: TwitchDeliveryStatus::AutoMod,
                message_id: None,
            })
        }),
        sessions(),
        1,
    )
    .unwrap();
    worker
        .enqueue(TwitchTestMessageRequest { request_id: 9 })
        .unwrap();
    assert_eq!(
        worker.wait_for_result(WAIT).unwrap().outcome,
        TwitchTestMessageOutcome::Dropped(TwitchDeliveryStatus::AutoMod)
    );
}

#[test]
fn test_worker_applies_input_and_output_backpressure_with_fifo_results() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let release_rx = Mutex::new(release_rx);
    let calls = Arc::new(AtomicUsize::new(0));
    let observed = calls.clone();
    let mut worker = TwitchTestMessageWorker::new(
        client(move |_, _, _, _| {
            let index = observed.fetch_add(1, Ordering::SeqCst);
            entered_tx.send(index).unwrap();
            if index == 0 {
                release_rx.lock().unwrap().recv_timeout(WAIT).unwrap();
            }
            Ok(sent())
        }),
        sessions(),
        1,
    )
    .unwrap();
    assert_eq!(
        worker.enqueue(TwitchTestMessageRequest { request_id: 0 }),
        Err(TwitchTestMessageDispatchError::InvalidRequest)
    );
    worker
        .enqueue(TwitchTestMessageRequest { request_id: 1 })
        .unwrap();
    assert_eq!(entered_rx.recv_timeout(WAIT).unwrap(), 0);
    worker
        .enqueue(TwitchTestMessageRequest { request_id: 2 })
        .unwrap();
    assert_eq!(
        worker.enqueue(TwitchTestMessageRequest { request_id: 3 }),
        Err(TwitchTestMessageDispatchError::QueueFull)
    );
    release_tx.send(()).unwrap();
    assert_eq!(entered_rx.recv_timeout(WAIT).unwrap(), 1);
    worker
        .enqueue(TwitchTestMessageRequest { request_id: 3 })
        .unwrap();
    assert_eq!(
        worker.enqueue(TwitchTestMessageRequest { request_id: 4 }),
        Err(TwitchTestMessageDispatchError::QueueFull)
    );
    let snapshot = worker.snapshot();
    assert_eq!(
        (
            snapshot.parallelism,
            snapshot.owned_threads,
            snapshot.results,
            snapshot.pending
        ),
        (1, 1, 1, 1)
    );
    assert_eq!(calls.load(Ordering::SeqCst), 2);
    for id in 1..=3 {
        assert_eq!(worker.wait_for_result(WAIT).unwrap().request_id, id);
    }
    assert_eq!(calls.load(Ordering::SeqCst), 3);
    worker.cancel_pending();
    worker.cancel_pending();
    assert!(worker.snapshot().stopping);
    assert_eq!(worker.snapshot().owned_threads, 1);
    assert_eq!(
        worker.enqueue(TwitchTestMessageRequest { request_id: 4 }),
        Err(TwitchTestMessageDispatchError::Stopping)
    );
}

#[test]
fn test_worker_destruction_joins_cooperatively_cancelled_chat() {
    let (entered_tx, entered_rx) = mpsc::channel();
    let finished = Arc::new(AtomicUsize::new(0));
    let observed = finished.clone();
    let mut worker = TwitchTestMessageWorker::new(
        client(move |_, _, _, stop| {
            entered_tx.send(()).unwrap();
            assert!(stop.wait_for_stop(WAIT));
            observed.fetch_add(1, Ordering::SeqCst);
            Err(transport(HttpErrorCode::Cancelled))
        }),
        sessions(),
        1,
    )
    .unwrap();
    worker
        .enqueue(TwitchTestMessageRequest { request_id: 1 })
        .unwrap();
    entered_rx.recv_timeout(WAIT).unwrap();
    worker
        .enqueue(TwitchTestMessageRequest { request_id: 2 })
        .unwrap();
    drop(worker);
    assert_eq!(finished.load(Ordering::SeqCst), 1);
}

#[derive(Default)]
struct MessengerState {
    requests: Vec<TwitchTestMessageRequest>,
    results: std::collections::VecDeque<TwitchTestMessageResult>,
    reject: bool,
    panic_enqueue: bool,
    panic_poll: bool,
    cancellations: usize,
}
struct Messenger(Arc<Mutex<MessengerState>>);
impl TwitchTestMessenger for Messenger {
    fn enqueue(
        &mut self,
        request: TwitchTestMessageRequest,
    ) -> Result<(), TwitchTestMessageDispatchError> {
        let mut state = self.0.lock().unwrap();
        if state.reject {
            return Err(TwitchTestMessageDispatchError::QueueFull);
        }
        state.requests.push(request);
        if state.panic_enqueue {
            drop(state);
            panic!("synthetic panic after admission");
        }
        Ok(())
    }
    fn try_take_result(&mut self) -> Option<TwitchTestMessageResult> {
        let mut state = self.0.lock().unwrap();
        if state.panic_poll {
            drop(state);
            panic!("synthetic result panic");
        }
        state.results.pop_front()
    }
    fn cancel_pending(&mut self) {
        let mut state = self.0.lock().unwrap();
        state.cancellations += 1;
        state.results.clear();
    }
}
fn connection(state: TwitchConnectionState) -> TwitchConnectionSnapshot {
    TwitchConnectionSnapshot {
        state,
        login: Some("broadcaster".into()),
        user_code: None,
        verification_uri: None,
        authorization_expires_at: None,
        access_expires_at: Some(SystemTime::UNIX_EPOCH + Duration::from_secs(20000)),
        diagnostic: None,
        revision: 2,
        shutting_down: false,
    }
}
fn controller() -> (TwitchTestMessageController, Arc<Mutex<MessengerState>>) {
    let state = Arc::new(Mutex::new(MessengerState::default()));
    (
        TwitchTestMessageController::new(Box::new(Messenger(state.clone()))),
        state,
    )
}

#[test]
fn test_controller_requires_connected_state_and_one_correlated_request() {
    let (mut controller, messenger) = controller();
    for state in [
        TwitchConnectionState::Disconnected,
        TwitchConnectionState::Starting,
        TwitchConnectionState::AwaitingUser,
        TwitchConnectionState::Validating,
        TwitchConnectionState::Refreshing,
        TwitchConnectionState::Disconnecting,
        TwitchConnectionState::Error,
        TwitchConnectionState::ShuttingDown,
    ] {
        assert_eq!(
            controller.send(&connection(state)),
            Err(TwitchTestMessageError::NotConnected)
        );
    }
    let connected = connection(TwitchConnectionState::Connected);
    assert_eq!(controller.send(&connected), Ok(1));
    assert_eq!(
        controller.send(&connected),
        Err(TwitchTestMessageError::Busy)
    );
    messenger
        .lock()
        .unwrap()
        .results
        .push_back(TwitchTestMessageResult {
            request_id: 999,
            outcome: TwitchTestMessageOutcome::Sent,
        });
    assert_eq!(controller.poll(), Err(TwitchTestMessageError::StaleResult));
    assert_eq!(controller.snapshot().state, TwitchTestMessageState::Sending);
    messenger
        .lock()
        .unwrap()
        .results
        .push_back(TwitchTestMessageResult {
            request_id: 1,
            outcome: TwitchTestMessageOutcome::Sent,
        });
    assert_eq!(controller.poll(), Ok(true));
    assert_eq!(controller.snapshot().state, TwitchTestMessageState::Sent);
    assert_eq!(
        controller.snapshot().delivery_status,
        Some(TwitchDeliveryStatus::Sent)
    );
    assert_eq!(controller.send(&connected), Ok(2));
    assert_eq!(controller.snapshot().delivery_status, None);
    assert_eq!(messenger.lock().unwrap().requests.len(), 2);
}

#[test]
fn test_controller_never_retries_ambiguous_or_safe_retry_results_automatically() {
    for (outcome, error) in [
        (
            TwitchTestMessageOutcome::RetryAfter(Duration::from_secs(1)),
            TwitchTestMessageError::RetrySuppressed,
        ),
        (
            TwitchTestMessageOutcome::Ambiguous,
            TwitchTestMessageError::Ambiguous,
        ),
        (
            TwitchTestMessageOutcome::Cancelled,
            TwitchTestMessageError::Cancelled,
        ),
        (
            TwitchTestMessageOutcome::Failed(TwitchChatError::ReconnectRequired),
            TwitchTestMessageError::Failed(TwitchChatError::ReconnectRequired),
        ),
        (
            TwitchTestMessageOutcome::Dropped(TwitchDeliveryStatus::AutoMod),
            TwitchTestMessageError::Dropped(TwitchDeliveryStatus::AutoMod),
        ),
        (
            TwitchTestMessageOutcome::Dropped(TwitchDeliveryStatus::Sent),
            TwitchTestMessageError::InvalidResult,
        ),
    ] {
        let (mut controller, messenger) = controller();
        let connected = connection(TwitchConnectionState::Connected);
        controller.send(&connected).unwrap();
        messenger
            .lock()
            .unwrap()
            .results
            .push_back(TwitchTestMessageResult {
                request_id: 1,
                outcome,
            });
        assert_eq!(controller.poll(), Err(error));
        assert_eq!(controller.snapshot().state, TwitchTestMessageState::Error);
        assert_eq!(controller.snapshot().diagnostic, Some(error));
        assert_eq!(controller.poll(), Ok(false));
        assert_eq!(messenger.lock().unwrap().requests.len(), 1);
        assert_eq!(controller.send(&connected), Ok(2));
    }
}

#[test]
fn test_controller_contains_dispatch_and_result_panics_and_cancels_once() {
    for case in 0..3 {
        let (mut controller, messenger) = controller();
        let connected = connection(TwitchConnectionState::Connected);
        match case {
            0 => {
                messenger.lock().unwrap().reject = true;
                assert_eq!(
                    controller.send(&connected),
                    Err(TwitchTestMessageError::DispatchFailed)
                );
            }
            1 => {
                messenger.lock().unwrap().panic_enqueue = true;
                assert_eq!(
                    controller.send(&connected),
                    Err(TwitchTestMessageError::Ambiguous)
                );
            }
            _ => {
                controller.send(&connected).unwrap();
                messenger.lock().unwrap().panic_poll = true;
                assert_eq!(controller.poll(), Err(TwitchTestMessageError::Ambiguous));
            }
        }
        controller.shutdown();
        controller.shutdown();
        assert_eq!(messenger.lock().unwrap().cancellations, 1);
        assert_eq!(
            controller.send(&connected),
            Err(TwitchTestMessageError::ShuttingDown)
        );
        assert_eq!(controller.poll(), Err(TwitchTestMessageError::ShuttingDown));
        assert_eq!(controller.snapshot().request_id, None);
    }
}

#[test]
fn a_completed_test_is_not_reclassified_by_polling_an_idle_messenger() {
    let (mut controller, messenger) = controller();
    controller
        .send(&connection(TwitchConnectionState::Connected))
        .unwrap();
    messenger
        .lock()
        .unwrap()
        .results
        .push_back(TwitchTestMessageResult {
            request_id: 1,
            outcome: TwitchTestMessageOutcome::Sent,
        });
    assert_eq!(controller.poll(), Ok(true));
    let completed = controller.snapshot();
    messenger.lock().unwrap().panic_poll = true;
    assert_eq!(controller.poll(), Ok(false));
    assert_eq!(controller.snapshot(), completed);
}
