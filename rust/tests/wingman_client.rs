use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;
use std::collections::VecDeque;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let folder = std::env::temp_dir().join(format!(
            "manny-wingman-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&folder).unwrap();
        let path = folder.join("sample-\u{96ea}.zevtc");
        fs::write(&path, b"fixture\0\xff").unwrap();
        Self(fs::canonicalize(path).unwrap())
    }
    fn identity(&self) -> LogFileIdentity {
        let m = fs::metadata(&self.0).unwrap();
        LogFileIdentity {
            canonical_path: self.0.clone(),
            size: m.len(),
            last_write_time: m.modified().unwrap(),
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn metadata() -> EncounterMetadata {
    EncounterMetadata {
        boss_id: 123,
        pov_account: "Player.1234".into(),
        remaining_health_basis_points: None,
    }
}
fn reply(status: u16, text: &str) -> Result<HttpResponse, HttpError> {
    Ok(HttpResponse {
        status_code: status,
        headers: Vec::new(),
        body: SecretValue::from_bytes(text.as_bytes().to_vec()),
    })
}
struct Random;
impl RandomSource for Random {
    fn fill(&self, bytes: &mut [u8]) -> Result<(), RandomError> {
        bytes.fill(1);
        Ok(())
    }
}
struct Time {
    origin: Instant,
    elapsed: AtomicU64,
    waits: Mutex<Vec<Duration>>,
    cancel: CancellationSource,
    cancel_wait: AtomicBool,
}
impl Time {
    fn new() -> Self {
        Self {
            origin: Instant::now(),
            elapsed: AtomicU64::new(0),
            waits: Mutex::new(Vec::new()),
            cancel: CancellationSource::default(),
            cancel_wait: AtomicBool::new(false),
        }
    }
}
impl PollClock for Time {
    fn now(&self) -> Instant {
        self.origin + Duration::from_millis(self.elapsed.load(Ordering::Relaxed))
    }
    fn wait(&self, duration: Duration, stop: &StopToken) -> bool {
        self.waits.lock().unwrap().push(duration);
        self.elapsed
            .fetch_add(duration.as_millis() as u64, Ordering::Relaxed);
        if self.cancel_wait.load(Ordering::Relaxed) {
            self.cancel.request_stop();
        }
        stop.is_stop_requested()
    }
}
struct Seen {
    method: HttpMethod,
    url: String,
    body: Vec<u8>,
    headers: Vec<(String, Vec<u8>)>,
    timeouts: HttpTimeouts,
    limits: HttpResponseLimits,
}
struct Http {
    responses: Mutex<VecDeque<Result<HttpResponse, HttpError>>>,
    seen: Mutex<Vec<Seen>>,
    pending: bool,
}
impl HttpClient for Http {
    fn execute(
        &self,
        mut request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, HttpError> {
        validate_http_request(&request, HttpTransportPolicy::HttpsOnly).unwrap();
        let mut body = Vec::new();
        if let Some(source) = &mut request.body {
            let length = source.content_length();
            let mut buffer = [0; 5];
            loop {
                let count = source.read(&mut buffer, stop).map_err(|e| HttpError {
                    body_error: Some(e.code),
                    ..HttpError::new(HttpErrorCode::BodyReadFailed)
                })?;
                if count == 0 {
                    break;
                }
                body.extend_from_slice(&buffer[..count]);
            }
            assert_eq!(body.len() as u64, length);
        }
        let ticket = request.url.starts_with(WINGMAN_TICKET_URL);
        self.seen.lock().unwrap().push(Seen {
            method: request.method,
            url: request.url,
            body,
            headers: request
                .headers
                .iter()
                .map(|h| (h.name().to_owned(), h.expose_value().to_vec()))
                .collect(),
            timeouts: request.timeouts,
            limits: request.response_limits,
        });
        if let Some(response) = self.responses.lock().unwrap().pop_front() {
            response
        } else if self.pending {
            reply(
                200,
                if ticket {
                    r#"{"state":"processing"}"#
                } else {
                    r#"{"success":false}"#
                },
            )
        } else {
            panic!("unexpected HTTP request")
        }
    }
}
fn setup(
    responses: Vec<Result<HttpResponse, HttpError>>,
    pending: bool,
    options: WingmanPollingOptions,
) -> (WingmanClient, Arc<Http>, Arc<Time>) {
    let http = Arc::new(Http {
        responses: Mutex::new(responses.into()),
        seen: Mutex::new(Vec::new()),
        pending,
    });
    let time = Arc::new(Time::new());
    let client = WingmanClient::with_options(
        http.clone(),
        Arc::new(Random),
        time.clone(),
        WINGMAN_UPLOAD_URL.into(),
        options,
    )
    .unwrap();
    (client, http, time)
}
fn upload(responses: Vec<Result<HttpResponse, HttpError>>) -> Result<WingmanSuccess, WingmanError> {
    let fixture = Fixture::new();
    let (client, _, time) = setup(responses, false, WingmanPollingOptions::default());
    client.upload(&fixture.identity(), &metadata(), &time.cancel.token())
}
fn import(responses: Vec<Result<HttpResponse, HttpError>>) -> Result<WingmanSuccess, WingmanError> {
    let (client, _, time) = setup(responses, false, WingmanPollingOptions::default());
    client.import_permalink("https://dps.report/fixture", &time.cancel.token())
}

#[test]
fn direct_legacy_upload_streams_ordered_fields_and_exact_file_bytes() {
    let fixture = Fixture::new();
    let (client, http, time) = setup(
        vec![reply(200, r#"{"result":true}"#)],
        false,
        WingmanPollingOptions::default(),
    );
    let result = client
        .upload(&fixture.identity(), &metadata(), &time.cancel.token())
        .unwrap();
    assert_eq!(
        result,
        WingmanSuccess {
            duplicate: false,
            permalink: None
        }
    );
    let seen = http.seen.lock().unwrap();
    assert_eq!(seen.len(), 1);
    let request = &seen[0];
    assert_eq!(request.method, HttpMethod::Post);
    assert_eq!(request.url, WINGMAN_UPLOAD_URL);
    assert_eq!(
        request.timeouts,
        HttpTimeouts {
            connect: Duration::from_secs(10),
            operation: Duration::from_secs(900),
            stalled_transfer: Duration::from_secs(900)
        }
    );
    assert_eq!(
        request.limits,
        HttpResponseLimits {
            max_header_bytes: 65536,
            max_body_bytes: 65536
        }
    );
    assert_eq!(
        request.headers[0],
        ("Accept".into(), b"application/json".to_vec())
    );
    let body = String::from_utf8_lossy(&request.body);
    let positions: Vec<_> = [
        "name=\"account\"",
        "name=\"filesize\"",
        "name=\"triggerID\"",
        "name=\"file\"",
    ]
    .iter()
    .map(|name| body.find(name).unwrap())
    .collect();
    assert!(positions.windows(2).all(|p| p[0] < p[1]));
    assert!(body.contains("\r\n\r\nPlayer.1234\r\n"));
    assert!(body.contains("\r\n\r\n9\r\n"));
    assert!(body.contains("\r\n\r\n123\r\n"));
    assert!(
        request
            .body
            .windows(9)
            .any(|bytes| bytes == b"fixture\0\xff")
    );
    assert!(time.waits.lock().unwrap().is_empty());
}

#[test]
fn ticket_processing_and_permalink_discovery_preserve_protocol_order() {
    let fixture = Fixture::new();
    let (client, http, time) = setup(
        vec![
            reply(200, r#"{"result":true,"ticket":42}"#),
            reply(200, r#"{"state":"queued"}"#),
            reply(200, r#"{"state":"deferred"}"#),
            reply(200, r#"{"state":"processing"}"#),
            reply(200, r#"{"state":"uploaded"}"#),
            reply(200, r#"{"success":false}"#),
            reply(200, r#"{"success":true,"log":{"html":"fixture-kill_1"}}"#),
        ],
        false,
        WingmanPollingOptions::default(),
    );
    let result = client
        .upload(&fixture.identity(), &metadata(), &time.cancel.token())
        .unwrap();
    assert_eq!(
        result.permalink,
        Some(format!("{WINGMAN_LOG_URL}fixture-kill_1"))
    );
    assert!(!result.duplicate);
    let seen = http.seen.lock().unwrap();
    assert_eq!(seen.len(), 7);
    for call in &seen[1..5] {
        assert_eq!(call.method, HttpMethod::Get);
        assert_eq!(call.url, format!("{WINGMAN_TICKET_URL}42"));
        assert!(call.body.is_empty());
        assert_eq!(call.timeouts.operation, Duration::from_secs(60));
    }
    for call in &seen[5..] {
        assert_eq!(call.url, WINGMAN_CHECK_UPLOAD_URL);
        assert_eq!(call.method, HttpMethod::Post);
        assert_eq!(
            call.body,
            b"file=Player1234_upload.zevtc&filesize=9&bossID=123&account=Player.1234"
        );
        assert!(
            call.headers
                .iter()
                .any(|(name, value)| name == "Content-Type"
                    && value == b"application/x-www-form-urlencoded")
        );
    }
    assert_eq!(
        *time.waits.lock().unwrap(),
        [
            Duration::from_secs(3),
            Duration::from_secs(3),
            Duration::from_secs(3),
            Duration::from_secs(3),
            Duration::from_secs(5)
        ]
    );
}

#[test]
fn duplicate_and_expired_tickets_complete_without_reuploading() {
    for first in [
        reply(409, "ignored private body"),
        reply(200, r#"{"result":true,"ticket":7}"#),
    ] {
        let duplicate = first.as_ref().unwrap().status_code == 409;
        let mut responses = vec![first];
        if !duplicate {
            responses.push(reply(404, "ignored private body"));
        }
        responses.push(reply(200, r#"{"success":true,"log":{"html":"existing"}}"#));
        let result = upload(responses).unwrap();
        assert_eq!(result.duplicate, duplicate);
        assert!(result.permalink.is_some());
    }
    assert!(
        upload(vec![
            reply(200, r#"{"result":true,"ticket":1}"#),
            reply(200, r#"{"state":"skipped"}"#),
            reply(200, r#"{"success":true,"log":{"html":"skip"}}"#)
        ])
        .is_ok()
    );
}

#[test]
fn phase_deadlines_stop_polling_and_cap_each_remaining_request() {
    let fixture = Fixture::new();
    let options = WingmanPollingOptions {
        ticket_timeout: Duration::from_secs(9),
        check_timeout: Duration::from_secs(10),
        ..WingmanPollingOptions::default()
    };
    let (client, http, time) = setup(
        vec![reply(200, r#"{"result":true,"ticket":3}"#)],
        true,
        options,
    );
    let result = client
        .upload(&fixture.identity(), &metadata(), &time.cancel.token())
        .unwrap();
    assert_eq!(
        result,
        WingmanSuccess {
            duplicate: false,
            permalink: None
        }
    );
    assert_eq!(time.elapsed.load(Ordering::Relaxed), 19000);
    let seen = http.seen.lock().unwrap();
    assert_eq!(seen.len(), 5);
    assert_eq!(
        seen[1..]
            .iter()
            .map(|r| r.timeouts.operation.as_secs())
            .collect::<Vec<_>>(),
        [6, 3, 10, 5]
    );
    assert_eq!(
        seen.iter().filter(|r| r.url == WINGMAN_UPLOAD_URL).count(),
        1
    );
}

#[test]
fn imports_encode_the_same_trusted_link_and_accept_only_confirmed_duplicates() {
    for queued in [0, 1] {
        for in_queue in [false, true] {
            for in_db in [false, true] {
                let (client, http, time) = setup(
                    vec![
                        reply(200, &format!("{{\"success\":{queued}}}")),
                        reply(
                            200,
                            &format!(
                                "{{\"inQueue\":{in_queue},\"inDB\":{in_db},\"targetURL\":\"{WINGMAN_LOG_URL}fixture\"}}"
                            ),
                        ),
                    ],
                    false,
                    WingmanPollingOptions::default(),
                );
                let result =
                    client.import_permalink("https://b.dps.report/log%20one", &time.cancel.token());
                if queued == 0 && !in_queue && !in_db {
                    assert_eq!(result.unwrap_err().code, WingmanErrorCode::InvalidResponse);
                } else {
                    let result = result.unwrap();
                    assert_eq!(result.duplicate, in_db);
                    assert!(result.permalink.is_some());
                }
                let seen = http.seen.lock().unwrap();
                assert_eq!(seen.len(), 2);
                for (index, prefix) in [WINGMAN_IMPORT_URL, WINGMAN_CHECK_IMPORT_URL]
                    .iter()
                    .enumerate()
                {
                    assert_eq!(
                        seen[index].url,
                        format!("{prefix}https%3A%2F%2Fb.dps.report%2Flog%2520one")
                    );
                    assert!(seen[index].body.is_empty());
                    assert_eq!(seen[index].timeouts.operation, Duration::from_secs(60));
                }
                assert_eq!(seen[0].method, HttpMethod::Post);
                assert_eq!(seen[1].method, HttpMethod::Get);
            }
        }
    }
}

#[test]
fn invalid_import_inputs_and_resolved_links_fail_without_crossing_origins() {
    let (client, http, time) = setup(vec![], false, WingmanPollingOptions::default());
    for url in [
        "http://dps.report/a".into(),
        "https://dps.report/".into(),
        "https://dps.report.evil/a".into(),
        "https://dps.report/a?secret=x".into(),
        "https://dps.report/a#x".into(),
        "https://dps.report/a@b".into(),
        "https://dps.report/a\\b".into(),
        "https://dps.report/\u{96ea}".into(),
        format!("https://dps.report/{}", ":".repeat(1900)),
    ] {
        assert_eq!(
            client
                .import_permalink(&url, &time.cancel.token())
                .unwrap_err()
                .code,
            WingmanErrorCode::InvalidInput
        );
    }
    assert!(http.seen.lock().unwrap().is_empty());
    for target in [
        format!("{WINGMAN_LOG_URL}../bad"),
        format!("{WINGMAN_LOG_URL}bad.html"),
        format!("{WINGMAN_LOG_URL}bad?x"),
        format!("{WINGMAN_LOG_URL}{}", "x".repeat(513)),
        "https://evil.invalid/log/x".into(),
    ] {
        assert!(
            import(vec![
                reply(200, r#"{"success":1}"#),
                reply(
                    200,
                    &format!("{{\"inQueue\":true,\"inDB\":false,\"targetURL\":\"{target}\"}}")
                )
            ])
            .is_err()
        );
    }
}

#[test]
fn all_response_shapes_are_bounded_strict_and_forward_compatible() {
    for text in [
        "garbage",
        "[]",
        "{}",
        r#"{"result":false}"#,
        r#"{"result":1}"#,
        r#"{"result":true,"ticket":0}"#,
        r#"{"result":true,"ticket":-1}"#,
        r#"{"result":true,"ticket":1.5}"#,
        r#"{"result":true,"result":false}"#,
        r#"{"result":true,"unknown":"\uD800"}"#,
    ] {
        assert!(upload(vec![reply(200, text)]).is_err());
    }
    assert!(
        upload(vec![reply(
            200,
            &" ".repeat(MAX_WINGMAN_RESPONSE_BYTES + 1)
        )])
        .is_err()
    );
    assert!(upload(vec![reply(200, r#"{"result":true,"unknown":1e400}"#)]).is_ok());
    for state in ["failed", "unknown", ""] {
        assert!(
            upload(vec![
                reply(200, r#"{"result":true,"ticket":1}"#),
                reply(200, &format!("{{\"state\":\"{state}\"}}"))
            ])
            .is_err()
        );
    }
    for check in [
        "{}",
        r#"{"success":1}"#,
        r#"{"success":true}"#,
        r#"{"success":true,"log":[]}"#,
        r#"{"success":true,"log":{"html":"../bad"}}"#,
    ] {
        assert!(upload(vec![reply(409, ""), reply(200, check)]).is_err());
    }
    for queued in [
        "{}",
        r#"{"success":true}"#,
        r#"{"success":2}"#,
        r#"{"success":-1}"#,
    ] {
        assert!(import(vec![reply(200, queued)]).is_err());
    }
    for checked in [
        "{}",
        r#"{"inQueue":true,"inDB":false}"#,
        r#"{"inQueue":1,"inDB":false,"targetURL":"x"}"#,
    ] {
        assert!(import(vec![reply(200, r#"{"success":1}"#), reply(200, checked)]).is_err());
    }
}

#[test]
fn status_and_transport_failures_keep_retry_rules_and_safe_diagnostics() {
    for (status, retry) in [
        (301, false),
        (400, false),
        (408, true),
        (429, true),
        (500, true),
        (599, true),
    ] {
        let error = upload(vec![reply(status, "private-response")]).unwrap_err();
        assert_eq!(
            error.disposition,
            if retry {
                WingmanDisposition::Retry
            } else {
                WingmanDisposition::Failed
            }
        );
        assert_eq!(
            error.retry_after,
            retry.then_some(Duration::from_secs(if status == 429 { 60 } else { 30 }))
        );
        assert_eq!(error.http_status, Some(status));
        assert!(!format!("{error}").contains("private-response"));
    }
    for (values, seconds) in [
        (vec!["1"], 1),
        (vec!["900"], 900),
        (vec!["901"], 60),
        (vec!["0"], 60),
        (vec!["+2"], 60),
        (vec!["date"], 60),
        (vec!["2", "3"], 60),
    ] {
        let mut r = reply(429, "").unwrap();
        r.headers = values
            .iter()
            .map(|v| HttpHeader::public("Retry-After", *v))
            .collect();
        assert_eq!(
            upload(vec![Ok(r)]).unwrap_err().retry_after,
            Some(Duration::from_secs(seconds))
        );
    }
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
        let retry = matches!(
            code,
            HttpErrorCode::Timeout
                | HttpErrorCode::NameResolutionFailed
                | HttpErrorCode::ConnectionFailed
                | HttpErrorCode::TlsFailed
                | HttpErrorCode::SendFailed
                | HttpErrorCode::ReceiveFailed
        );
        let error = import(vec![Err(HttpError::new(code))]).unwrap_err();
        assert_eq!(error.http_error, Some(code));
        assert_eq!(
            error.disposition,
            if code == HttpErrorCode::Cancelled {
                WingmanDisposition::Cancelled
            } else if retry {
                WingmanDisposition::Retry
            } else {
                WingmanDisposition::Failed
            }
        );
    }
}

#[test]
fn input_endpoint_and_polling_validation_precede_network_work() {
    let fixture = Fixture::new();
    let (client, http, time) = setup(vec![], false, WingmanPollingOptions::default());
    for mode in 0..6 {
        let mut file = fixture.identity();
        let mut metadata = metadata();
        match mode {
            0 => file.size = 0,
            1 => metadata.boss_id = 0,
            2 => metadata.boss_id = WVW_BOSS_ID,
            3 => metadata.pov_account.clear(),
            4 => metadata.pov_account = "a".repeat(257),
            _ => metadata.pov_account = "private\naccount".into(),
        }
        assert_eq!(
            client
                .upload(&file, &metadata, &time.cancel.token())
                .unwrap_err()
                .code,
            WingmanErrorCode::InvalidInput
        );
    }
    let mut changed = fixture.identity();
    changed.size += 1;
    assert_eq!(
        client
            .upload(&changed, &metadata(), &time.cancel.token())
            .unwrap_err()
            .code,
        WingmanErrorCode::FileUnavailable
    );
    for url in [
        "http://evtc.bel.st/evtc",
        "https://user@evtc.bel.st/evtc",
        "https://evtc.bel.st/evtc#fragment",
        "https://bad:invalid/evtc",
    ] {
        assert!(
            WingmanClient::with_options(
                http.clone(),
                Arc::new(Random),
                time.clone(),
                url.into(),
                WingmanPollingOptions::default()
            )
            .is_err()
        );
    }
    for options in [
        WingmanPollingOptions {
            ticket_interval: Duration::ZERO,
            ..WingmanPollingOptions::default()
        },
        WingmanPollingOptions {
            check_timeout: Duration::from_secs(301),
            ..WingmanPollingOptions::default()
        },
    ] {
        assert!(
            WingmanClient::with_options(
                http.clone(),
                Arc::new(Random),
                time.clone(),
                WINGMAN_UPLOAD_URL.into(),
                options
            )
            .is_err()
        );
    }
    assert!(http.seen.lock().unwrap().is_empty());
}

#[test]
fn cancellation_interrupts_polling_without_later_requests() {
    let fixture = Fixture::new();
    let (client, http, time) = setup(
        vec![reply(200, r#"{"result":true,"ticket":1}"#)],
        false,
        WingmanPollingOptions::default(),
    );
    time.cancel_wait.store(true, Ordering::Relaxed);
    assert_eq!(
        client
            .upload(&fixture.identity(), &metadata(), &time.cancel.token())
            .unwrap_err()
            .disposition,
        WingmanDisposition::Cancelled
    );
    assert_eq!(http.seen.lock().unwrap().len(), 1);
    assert_eq!(
        client
            .import_permalink("https://dps.report/fixture", &time.cancel.token())
            .unwrap_err()
            .disposition,
        WingmanDisposition::Cancelled
    );
    assert_eq!(http.seen.lock().unwrap().len(), 1);
    let source = CancellationSource::default();
    let stop = source.token();
    let (started, ready) = std::sync::mpsc::sync_channel(1);
    let worker = std::thread::spawn(move || {
        started.send(()).unwrap();
        SystemPollClock.wait(Duration::from_secs(30), &stop)
    });
    ready.recv_timeout(Duration::from_secs(5)).unwrap();
    let begin = Instant::now();
    source.request_stop();
    assert!(worker.join().unwrap());
    assert!(begin.elapsed() < Duration::from_secs(1));
}

struct Transport<F>(F);
impl<F> HttpClient for Transport<F>
where
    F: Fn(HttpRequest, &StopToken) -> Result<HttpResponse, HttpError> + Send + Sync,
{
    fn execute(&self, request: HttpRequest, stop: &StopToken) -> Result<HttpResponse, HttpError> {
        (self.0)(request, stop)
    }
}

#[test]
fn file_changes_random_failures_and_transport_panics_remain_contained() {
    let fixture = Fixture::new();
    let file = fixture.identity();
    let path = fixture.0.clone();
    let (_, http, _) = setup(vec![], false, WingmanPollingOptions::default());
    let client = WingmanClient::new(
        Arc::new(Transport(move |request: HttpRequest, stop: &StopToken| {
            fs::write(&path, b"changed").unwrap();
            http.execute(request, stop)
        })),
        Arc::new(Random),
    );
    let stop = CancellationSource::default().token();
    assert_eq!(
        client
            .upload(&file, &metadata(), &stop)
            .unwrap_err()
            .http_error,
        Some(HttpErrorCode::BodyReadFailed)
    );
    let throwing = Arc::new(Transport(
        |_: HttpRequest, _: &StopToken| -> Result<HttpResponse, HttpError> {
            panic!("synthetic private failure")
        },
    ));
    let client = WingmanClient::new(throwing.clone(), Arc::new(Random));
    for error in [
        client
            .upload(&fixture.identity(), &metadata(), &stop)
            .unwrap_err(),
        client
            .import_permalink("https://dps.report/fixture", &stop)
            .unwrap_err(),
    ] {
        assert_eq!(error.code, WingmanErrorCode::Internal);
        assert!(!format!("{error}").contains("private"));
    }
    struct NoRandom;
    impl RandomSource for NoRandom {
        fn fill(&self, _: &mut [u8]) -> Result<(), RandomError> {
            Err(RandomError::Unavailable)
        }
    }
    let client = WingmanClient::new(throwing, Arc::new(NoRandom));
    assert_eq!(
        client
            .upload(&fixture.identity(), &metadata(), &stop)
            .unwrap_err()
            .code,
        WingmanErrorCode::MultipartFailed
    );
}

#[test]
fn cancellation_after_http_never_publishes_success_or_starts_a_followup() {
    for imported in [false, true] {
        let cancel = Arc::new(CancellationSource::default());
        let during = cancel.clone();
        let client = WingmanClient::new(
            Arc::new(Transport(move |_: HttpRequest, _: &StopToken| {
                during.request_stop();
                reply(
                    200,
                    if imported {
                        r#"{"success":1}"#
                    } else {
                        r#"{"result":true}"#
                    },
                )
            })),
            Arc::new(Random),
        );
        let fixture = Fixture::new();
        let result = if imported {
            client.import_permalink("https://dps.report/fixture", &cancel.token())
        } else {
            client.upload(&fixture.identity(), &metadata(), &cancel.token())
        };
        assert_eq!(
            result.unwrap_err().disposition,
            WingmanDisposition::Cancelled
        );
    }
}

#[test]
fn unicode_accounts_are_preserved_in_multipart_and_encoded_in_discovery() {
    let fixture = Fixture::new();
    let (client, http, time) = setup(
        vec![
            reply(409, ""),
            reply(200, r#"{"success":true,"log":{"html":"fixture"}}"#),
        ],
        false,
        WingmanPollingOptions::default(),
    );
    let mut metadata = metadata();
    metadata.pov_account = "\u{96ea} &+.1234".into();
    client
        .upload(&fixture.identity(), &metadata, &time.cancel.token())
        .unwrap();
    let seen = http.seen.lock().unwrap();
    assert!(
        seen[0]
            .body
            .windows(metadata.pov_account.len())
            .any(|bytes| bytes == metadata.pov_account.as_bytes())
    );
    assert_eq!(seen[1].body,b"file=%E9%9B%AA%20%26%2B1234_upload.zevtc&filesize=9&bossID=123&account=%E9%9B%AA%20%26%2B.1234");
}
