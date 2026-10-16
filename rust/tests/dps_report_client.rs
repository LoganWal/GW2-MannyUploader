use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::time::Duration;

use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;
use serde_json::json;

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let directory = std::env::temp_dir().join(format!(
            "manny-dps-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&directory).unwrap();
        let path = directory.join("fixture-\u{96ea}.zevtc");
        fs::write(&path, b"fixture\0\xff").unwrap();
        Self(fs::canonicalize(path).unwrap())
    }
    fn identity(&self) -> LogFileIdentity {
        let metadata = fs::metadata(&self.0).unwrap();
        LogFileIdentity {
            canonical_path: self.0.clone(),
            size: metadata.len(),
            last_write_time: metadata.modified().unwrap(),
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(self.0.parent().unwrap());
    }
}
struct Random;
impl RandomSource for Random {
    fn fill(&self, bytes: &mut [u8]) -> Result<(), RandomError> {
        bytes.fill(0xab);
        Ok(())
    }
}
struct Http<F>(F);
impl<F> HttpClient for Http<F>
where
    F: Fn(HttpRequest, &StopToken) -> Result<HttpResponse, HttpError> + Send + Sync,
{
    fn execute(&self, request: HttpRequest, stop: &StopToken) -> Result<HttpResponse, HttpError> {
        (self.0)(request, stop)
    }
}
fn client(
    f: impl Fn(HttpRequest, &StopToken) -> Result<HttpResponse, HttpError> + Send + Sync + 'static,
) -> DpsReportClient {
    DpsReportClient::new(Arc::new(Http(f)), Arc::new(Random))
}
fn response(status: u16, bytes: Vec<u8>) -> HttpResponse {
    HttpResponse {
        status_code: status,
        headers: Vec::new(),
        body: SecretValue::from_bytes(bytes),
    }
}
fn valid() -> serde_json::Value {
    json!({"permalink":"https://dps.report/fixture", "encounter":{"bossId":123,"boss":"Boss","success":true}})
}
fn run(
    document: serde_json::Value,
    token: Option<&SecretValue>,
) -> Result<DpsReportSuccess, DpsReportError> {
    run_bytes(serde_json::to_vec(&document).unwrap(), token)
}
fn run_bytes(
    bytes: Vec<u8>,
    token: Option<&SecretValue>,
) -> Result<DpsReportSuccess, DpsReportError> {
    let fixture = Fixture::new();
    client(move |_, _| Ok(response(200, bytes.clone()))).upload(
        &fixture.identity(),
        token,
        DpsReportOptions::default(),
        &CancellationSource::default().token(),
    )
}
fn drain(request: &mut HttpRequest, stop: &StopToken) -> Result<Vec<u8>, HttpError> {
    let body = request.body.as_mut().unwrap();
    let length = body.content_length();
    let mut bytes = Vec::new();
    let mut chunk = [0; 7];
    loop {
        let count = body.read(&mut chunk, stop).map_err(|error| HttpError {
            body_error: Some(error.code),
            ..HttpError::new(HttpErrorCode::BodyReadFailed)
        })?;
        if count == 0 {
            break;
        }
        bytes.extend_from_slice(&chunk[..count]);
    }
    assert_eq!(bytes.len() as u64, length);
    Ok(bytes)
}

#[test]
fn wire_requests_capture_options_and_stream_tokens_only_in_the_form() {
    for detailed in [false, true] {
        for with_token in [false, true] {
            let fixture = Fixture::new();
            let client = client(move |mut request, stop| {
                validate_http_request(&request, HttpTransportPolicy::HttpsOnly).unwrap();
                assert_eq!(request.method, HttpMethod::Post);
                assert_eq!(
                    request.url,
                    if detailed {
                        DPS_REPORT_DETAILED_WVW_URL
                    } else {
                        DPS_REPORT_UPLOAD_URL
                    }
                );
                assert_eq!(request.headers.len(), 2);
                assert_eq!(request.headers[0].name(), "Accept");
                assert_eq!(request.headers[0].expose_value(), b"application/json");
                assert_eq!(
                    request.timeouts,
                    HttpTimeouts {
                        connect: Duration::from_secs(10),
                        operation: Duration::from_secs(900),
                        stalled_transfer: Duration::from_secs(900)
                    }
                );
                assert_eq!(
                    request.response_limits,
                    HttpResponseLimits {
                        max_header_bytes: 65536,
                        max_body_bytes: 1048576
                    }
                );
                assert!(!format!("{request:?}").contains("synthetic-token"));
                let body = drain(&mut request, stop)?;
                let body = String::from_utf8_lossy(&body);
                assert_eq!(body.contains("name=\"userToken\""), with_token);
                assert_eq!(body.contains("synthetic-token"), with_token);
                assert!(body.contains("name=\"file\"; filename=\"upload.zevtc\"\r\nContent-Type: application/octet-stream\r\n\r\nfixture\0"));
                assert!(body.ends_with(&format!(
                    "\r\n--------------------------manny-{}--\r\n",
                    "ab".repeat(16)
                )));
                Ok(response(200, serde_json::to_vec(&valid()).unwrap()))
            });
            let token = SecretValue::from_bytes(b"synthetic-token".to_vec());
            let result = client
                .upload(
                    &fixture.identity(),
                    with_token.then_some(&token),
                    DpsReportOptions {
                        detailed_wvw: detailed,
                    },
                    &CancellationSource::default().token(),
                )
                .unwrap();
            assert_eq!(result.report.boss_id, 123);
        }
    }
}

#[test]
fn modes_follow_precedence_and_preserve_utf8_and_failed_encounters() {
    for (legendary, cm, emboldened, expected) in [
        (true, true, 5, "LCM"),
        (false, true, 5, "CM"),
        (false, false, 5, "Emboldened 5"),
        (false, false, 0, ""),
        (false, false, -1, ""),
    ] {
        let mut document = valid();
        document["encounter"]["boss"] = json!("Boss \u{96ea}");
        document["encounter"]["success"] = json!(false);
        document["encounter"]["isLegendaryCm"] = json!(legendary);
        document["encounter"]["isCm"] = json!(cm);
        document["encounter"]["emboldened"] = json!(emboldened);
        let result = run(document, None).unwrap();
        assert_eq!(result.report.mode, expected);
        assert_eq!(result.report.encounter_name, "Boss \u{96ea}");
        assert!(!result.report.success);
    }
}

#[test]
fn returned_tokens_remain_separate_wiping_values_and_do_not_erase_existing_tokens() {
    let current = SecretValue::from_bytes(b"synthetic-current".to_vec());
    for value in [json!(null), json!(""), json!("synthetic-current")] {
        let mut document = valid();
        document["userToken"] = value;
        assert!(
            run(document, Some(&current))
                .unwrap()
                .replacement_user_token
                .is_none()
        );
    }
    let mut document = valid();
    document["userToken"] = json!("synthetic-new");
    let result = run(document, Some(&current)).unwrap();
    assert!(result.replacement_user_token.unwrap().expose() == b"synthetic-new");
    for invalid in [
        "a".repeat(257),
        "a b".into(),
        "line\nbreak".into(),
        "\u{96ea}".into(),
    ] {
        let mut document = valid();
        document["userToken"] = json!(invalid);
        let result = run(document, Some(&current)).unwrap();
        assert_eq!(result.warning, Some(DpsReportWarning::InvalidReturnedToken));
        assert!(result.replacement_user_token.is_none());
    }
    let mut document = valid();
    document["error"] = json!("private-server-detail");
    document["userToken"] = json!("a".repeat(256));
    let result = run(document, None).unwrap();
    assert_eq!(result.warning, Some(DpsReportWarning::GeneratedWithWarning));
    assert!(!format!("{result:?}").contains("private-server-detail"));
    assert!(!format!("{result:?}").contains(&"a".repeat(256)));
}

#[test]
fn malformed_missing_duplicate_and_wrong_typed_fields_are_permanent_failures() {
    for bytes in [
        b"{".to_vec(),
        b"[]".to_vec(),
        b"null".to_vec(),
        b"{}".to_vec(),
        b"{\"permalink\":\"x\",\"permalink\":\"y\"}".to_vec(),
        [serde_json::to_vec(&valid()).unwrap(), b" trailing".to_vec()].concat(),
    ] {
        assert_eq!(
            run_bytes(bytes, None).unwrap_err().code,
            DpsReportErrorCode::InvalidResponse
        );
    }
    for path in ["permalink", "encounter", "userToken", "error"] {
        let mut document = valid();
        document[path] = json!(42);
        assert_eq!(
            run(document, None).unwrap_err().code,
            DpsReportErrorCode::InvalidResponse
        );
    }
    for (field, value) in [
        ("success", json!("true")),
        ("bossId", json!(65536)),
        ("bossId", json!(-1)),
        ("bossId", json!(1.5)),
        ("boss", json!("")),
        ("boss", json!("bad\nname")),
        ("boss", json!("x".repeat(257))),
        ("isCm", json!(1)),
        ("isLegendaryCm", json!("yes")),
        ("emboldened", json!(2147483648i64)),
    ] {
        let mut document = valid();
        document["encounter"][field] = value;
        let error = run(document, None).unwrap_err();
        assert_eq!(error.disposition, DpsReportDisposition::Failed);
        assert_eq!(error.http_status, Some(200));
    }
    for field in ["success", "bossId", "boss"] {
        let mut document = valid();
        document["encounter"].as_object_mut().unwrap().remove(field);
        assert!(run(document, None).is_err());
    }
    let mut document = valid();
    document["encounter"] = json!([true, 123, "Boss"]);
    assert!(run(document, None).is_err());
}

#[test]
fn permalinks_require_trusted_origins_and_wvw_boss_identity() {
    for url in [
        "https://dps.report/a",
        "https://b.dps.report/a",
        "https://wvw.report/a",
    ] {
        let mut document = valid();
        document["permalink"] = json!(url);
        document["encounter"]["bossId"] = json!(1);
        assert!(run(document, None).is_ok());
    }
    for url in [
        "https://wvw.report/a".into(),
        "https://dps.report/".into(),
        "https://dps.report.evil/a".into(),
        "https://user@dps.report/a".into(),
        "https://dps.report/a@b".into(),
        "http://dps.report/a".into(),
        "https://dps.report/a?private=value".into(),
        "https://dps.report/a#x".into(),
        "https://dps.report/a\\b".into(),
        "https://dps.report/a\"b".into(),
        "https://dps.report/\u{96ea}".into(),
        format!("https://dps.report/{}", "a".repeat(2048)),
    ] {
        let mut document = valid();
        document["permalink"] = json!(url);
        assert_eq!(
            run(document, None).unwrap_err().code,
            DpsReportErrorCode::InvalidResponse
        );
    }
}

#[test]
fn response_size_utf8_and_unknown_field_syntax_are_checked() {
    assert!(run_bytes(vec![b' '; MAX_DPS_REPORT_RESPONSE_BYTES + 1], None).is_err());
    let mut bytes = serde_json::to_vec(&valid()).unwrap();
    bytes.pop();
    bytes.extend_from_slice(b",\"unknown\":\"\xff\"}");
    assert!(run_bytes(bytes, None).is_err());
    let mut document = valid();
    document["unknown"] = json!({"nested":[1,true,"ok"]});
    assert!(run(document, None).is_ok());
    let mut bytes = serde_json::to_vec(&valid()).unwrap();
    bytes.pop();
    bytes.extend_from_slice(b",\"unknown\":[bad]} ");
    assert!(run_bytes(bytes, None).is_err());
    let mut bytes = serde_json::to_vec(&valid()).unwrap();
    bytes.pop();
    bytes.extend_from_slice(b",\"unknown\":\"\\uD800\"}");
    assert!(run_bytes(bytes, None).is_err());
    for extra in [
        b",\"unknown\":1e400}".as_slice(),
        b",\"unknown\":\"\\uD83D\\uDE00\"}",
        b",\"unknown\":\"\\\\uD800\"}",
    ] {
        let mut bytes = serde_json::to_vec(&valid()).unwrap();
        bytes.pop();
        bytes.extend_from_slice(extra);
        assert!(run_bytes(bytes, None).is_ok());
    }
}

#[test]
fn statuses_and_retry_after_are_bounded_and_duplicate_safe() {
    let fixture = Fixture::new();
    for (status, retry) in [
        (199, false),
        (300, false),
        (301, false),
        (400, false),
        (401, false),
        (408, true),
        (429, true),
        (500, true),
        (599, true),
        (600, false),
    ] {
        let client = client(move |_, _| Ok(response(status, b"private-response".to_vec())));
        let error = client
            .upload(
                &fixture.identity(),
                None,
                DpsReportOptions::default(),
                &CancellationSource::default().token(),
            )
            .unwrap_err();
        assert_eq!(
            error.disposition,
            if retry {
                DpsReportDisposition::Retry
            } else {
                DpsReportDisposition::Failed
            }
        );
        assert_eq!(
            error.retry_after,
            retry.then_some(Duration::from_secs(if status == 429 { 60 } else { 30 }))
        );
        assert!(!format!("{error}").contains("private-response"));
    }
    for (values, seconds) in [
        (vec!["1"], 1),
        (vec!["900"], 900),
        (vec!["090"], 90),
        (vec!["901"], 60),
        (vec!["0"], 60),
        (vec![""], 60),
        (vec!["+2"], 60),
        (vec![" 2"], 60),
        (vec!["2", "3"], 60),
        (vec!["date"], 60),
        (vec!["9999999999999999999999999"], 60),
    ] {
        let client = client(move |_, _| {
            let mut r = response(429, vec![]);
            r.headers = values
                .iter()
                .map(|value| HttpHeader::public("rEtRy-AfTeR", *value))
                .collect();
            Ok(r)
        });
        assert_eq!(
            client
                .upload(
                    &fixture.identity(),
                    None,
                    DpsReportOptions::default(),
                    &CancellationSource::default().token()
                )
                .unwrap_err()
                .retry_after,
            Some(Duration::from_secs(seconds))
        );
    }
}

#[test]
fn all_transport_categories_keep_only_safe_classification() {
    let fixture = Fixture::new();
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
        let client = client(move |_, _| Err(HttpError::new(code)));
        let error = client
            .upload(
                &fixture.identity(),
                None,
                DpsReportOptions::default(),
                &CancellationSource::default().token(),
            )
            .unwrap_err();
        assert_eq!(error.http_error, Some(code));
        assert_eq!(
            error.disposition,
            if code == HttpErrorCode::Cancelled {
                DpsReportDisposition::Cancelled
            } else if retry {
                DpsReportDisposition::Retry
            } else {
                DpsReportDisposition::Failed
            }
        );
        assert_eq!(error.retry_after, retry.then_some(Duration::from_secs(30)));
    }
}

#[test]
fn invalid_tokens_files_and_precancellation_do_not_start_network_work() {
    let fixture = Fixture::new();
    let calls = Arc::new(AtomicUsize::new(0));
    let counter = calls.clone();
    let client = client(move |_, _| {
        counter.fetch_add(1, Ordering::Relaxed);
        panic!("network must not start")
    });
    for bytes in [
        vec![],
        b"space token".to_vec(),
        vec![b'a'; 257],
        vec![0],
        vec![255],
    ] {
        let token = SecretValue::from_bytes(bytes);
        assert_eq!(
            client
                .upload(
                    &fixture.identity(),
                    Some(&token),
                    DpsReportOptions::default(),
                    &CancellationSource::default().token()
                )
                .unwrap_err()
                .code,
            DpsReportErrorCode::InvalidToken
        );
    }
    for change in 0..4 {
        let mut file = fixture.identity();
        match change {
            0 => file.size = 0,
            1 => file.size += 1,
            2 => file.last_write_time += Duration::from_secs(1),
            _ => {
                file.canonical_path.set_extension("missing");
            }
        }
        assert_eq!(
            client
                .upload(
                    &file,
                    None,
                    DpsReportOptions::default(),
                    &CancellationSource::default().token()
                )
                .unwrap_err()
                .code,
            DpsReportErrorCode::FileUnavailable
        );
    }
    let cancel = CancellationSource::default();
    cancel.request_stop();
    assert_eq!(
        client
            .upload(
                &fixture.identity(),
                None,
                DpsReportOptions::default(),
                &cancel.token()
            )
            .unwrap_err()
            .disposition,
        DpsReportDisposition::Cancelled
    );
    assert_eq!(calls.load(Ordering::Relaxed), 0);
}

#[test]
fn file_changes_during_streaming_and_transport_panics_fail_safely() {
    let fixture = Fixture::new();
    let file = fixture.identity();
    let path = fixture.0.clone();
    let client = client(move |mut request, stop| {
        fs::write(&path, b"changed").unwrap();
        drain(&mut request, stop)?;
        Ok(response(200, vec![]))
    });
    assert_eq!(
        client
            .upload(
                &file,
                None,
                DpsReportOptions::default(),
                &CancellationSource::default().token()
            )
            .unwrap_err()
            .http_error,
        Some(HttpErrorCode::BodyReadFailed)
    );
    let throwing = DpsReportClient::new(
        Arc::new(Http(
            |_: HttpRequest, _: &StopToken| -> Result<HttpResponse, HttpError> {
                panic!("private exception")
            },
        )),
        Arc::new(Random),
    );
    let error = throwing
        .upload(
            &fixture.identity(),
            None,
            DpsReportOptions::default(),
            &CancellationSource::default().token(),
        )
        .unwrap_err();
    assert_eq!(error.code, DpsReportErrorCode::Internal);
    assert!(!format!("{error}").contains("private exception"));
}

#[test]
fn random_failure_and_cancellation_after_http_do_not_return_success() {
    struct NoRandom;
    impl RandomSource for NoRandom {
        fn fill(&self, _: &mut [u8]) -> Result<(), RandomError> {
            Err(RandomError::Unavailable)
        }
    }
    let fixture = Fixture::new();
    let unavailable = DpsReportClient::new(
        Arc::new(Http(
            |_: HttpRequest, _: &StopToken| -> Result<HttpResponse, HttpError> {
                panic!("network must not start")
            },
        )),
        Arc::new(NoRandom),
    );
    assert_eq!(
        unavailable
            .upload(
                &fixture.identity(),
                None,
                DpsReportOptions::default(),
                &CancellationSource::default().token()
            )
            .unwrap_err()
            .code,
        DpsReportErrorCode::MultipartFailed
    );
    let cancel = Arc::new(CancellationSource::default());
    let during = cancel.clone();
    let client = client(move |_, _| {
        during.request_stop();
        Ok(response(200, serde_json::to_vec(&valid()).unwrap()))
    });
    assert_eq!(
        client
            .upload(
                &fixture.identity(),
                None,
                DpsReportOptions::default(),
                &cancel.token()
            )
            .unwrap_err()
            .disposition,
        DpsReportDisposition::Cancelled
    );
}

#[test]
fn all_json_strings_require_paired_unicode_escapes_even_in_unknown_fields() {
    for escaped in [
        "\\uD800",
        "\\uDBFF",
        "\\uDC00",
        "\\uDFFF",
        "\\uD800\\u0041",
        "\\uD800\\uD800",
        "\\uD800x\\uDC00",
        "\\uZZZZ",
    ] {
        let mut bytes = serde_json::to_vec(&valid()).unwrap();
        bytes.pop();
        bytes.extend_from_slice(
            format!(",\"unknown\":{{\"nested\":[\"{escaped}\"]}}}} ").as_bytes(),
        );
        assert!(run_bytes(bytes, None).is_err());
    }
    for escaped in [
        "\\uD800\\uDC00",
        "\\uDBFF\\uDFFF",
        "\\ud83d\\ude00",
        "\\u0041",
        "\\u0000",
        "\\\\uD800",
        "\\\"quoted\\\"",
    ] {
        let mut bytes = serde_json::to_vec(&valid()).unwrap();
        bytes.pop();
        bytes.extend_from_slice(format!(",\"unknown\":\"{escaped}\"}} ").as_bytes());
        assert!(run_bytes(bytes, None).is_ok());
    }
}
