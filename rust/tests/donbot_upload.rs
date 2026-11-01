use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::providers::*;
use serde_json::{Value, json};
use std::collections::VecDeque;
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const BASE: &str = "https://donbot.example/service";
const PAYLOAD: &[u8] = b"fixture\0\xff";
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "manny-donbot-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&dir).unwrap();
        let path = dir.join("fixture-\u{96ea}.zevtc");
        fs::write(&path, PAYLOAD).unwrap();
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
struct Seen {
    method: HttpMethod,
    url: String,
    headers: Vec<(String, Vec<u8>, HttpHeaderSensitivity)>,
    body: Vec<u8>,
    timeouts: HttpTimeouts,
    limits: HttpResponseLimits,
}
struct Http {
    replies: Mutex<VecDeque<Result<HttpResponse, HttpError>>>,
    seen: Mutex<Vec<Seen>>,
    cancel: CancellationSource,
    cancel_on: Option<usize>,
    panic_on: Option<usize>,
    change_on: Option<(usize, PathBuf)>,
}
impl HttpClient for Http {
    fn execute(
        &self,
        mut request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, HttpError> {
        validate_http_request(&request, HttpTransportPolicy::HttpsOnly).unwrap();
        let index = self.seen.lock().unwrap().len();
        assert!(!format!("{request:?}").contains("SYNTHETIC-KEY"));
        if self.panic_on == Some(index) {
            panic!("synthetic private panic");
        }
        if let Some((at, path)) = &self.change_on {
            if *at == index {
                fs::write(path, b"changed").unwrap();
            }
        }
        let mut body = Vec::new();
        if let Some(source) = &mut request.body {
            let mut chunk = [0; 4];
            loop {
                let count = source.read(&mut chunk, stop).map_err(|e| HttpError {
                    body_error: Some(e.code),
                    ..HttpError::new(HttpErrorCode::BodyReadFailed)
                })?;
                if count == 0 {
                    break;
                }
                body.extend_from_slice(&chunk[..count]);
            }
            assert_eq!(body.len() as u64, source.content_length());
        }
        self.seen.lock().unwrap().push(Seen {
            method: request.method,
            url: request.url,
            headers: request
                .headers
                .iter()
                .map(|h| (h.name().into(), h.expose_value().to_vec(), h.sensitivity()))
                .collect(),
            body,
            timeouts: request.timeouts,
            limits: request.response_limits,
        });
        if self.cancel_on == Some(index) {
            self.cancel.request_stop();
        }
        self.replies
            .lock()
            .unwrap()
            .pop_front()
            .expect("scripted HTTP response")
    }
}
fn setup(replies: Vec<Result<HttpResponse, HttpError>>) -> (DonBotClient, Arc<Http>) {
    configured(replies, None, None, None)
}
fn configured(
    replies: Vec<Result<HttpResponse, HttpError>>,
    cancel_on: Option<usize>,
    panic_on: Option<usize>,
    change_on: Option<(usize, PathBuf)>,
) -> (DonBotClient, Arc<Http>) {
    let http = Arc::new(Http {
        replies: Mutex::new(replies.into()),
        seen: Mutex::new(vec![]),
        cancel: CancellationSource::default(),
        cancel_on,
        panic_on,
        change_on,
    });
    (DonBotClient::new(http.clone()), http)
}
fn key() -> SecretValue {
    SecretValue::from_bytes(b"SYNTHETIC-KEY".to_vec())
}
fn options(route: DonBotDeliveryRoute) -> DonBotOptions {
    DonBotOptions {
        api_base_url: format!("{BASE}///"),
        guild_id: "123".into(),
        delivery: route,
    }
}
fn reply(status: u16, body: &str) -> Result<HttpResponse, HttpError> {
    Ok(HttpResponse {
        status_code: status,
        headers: vec![],
        body: SecretValue::from_bytes(body.as_bytes().to_vec()),
    })
}
fn create(location: &str, tracked: bool, delivery: bool) -> Result<HttpResponse, HttpError> {
    let mut r = reply(201, "").unwrap();
    r.headers = vec![
        HttpHeader::public("Location", location),
        HttpHeader::public("Tus-Resumable", "1.0.0"),
    ];
    if tracked {
        r.headers.push(HttpHeader::public("X-Log-Upload-Id", "42"));
    }
    if delivery {
        r.headers
            .push(HttpHeader::public("X-DonBot-Discord-Delivery", "accepted"));
    }
    Ok(r)
}
fn patched() -> Result<HttpResponse, HttpError> {
    let mut r = reply(204, "").unwrap();
    r.headers = vec![
        HttpHeader::public("Tus-Resumable", "1.0.0"),
        HttpHeader::public("Upload-Offset", PAYLOAD.len().to_string()),
    ];
    Ok(r)
}
fn receipt(requested: bool, outcome: &str, counts: [u16; 4]) -> Value {
    json!({"requested":requested,"outcome":outcome,"sent":counts[0],"skipped":counts[1],"failed":counts[2],"ambiguous":counts[3]})
}
fn completed(delivery: Option<Value>) -> String {
    let mut value = json!({"stage":"complete","fightLogId":99});
    if let Some(delivery) = delivery {
        value["discordDelivery"] = delivery;
    }
    format!("data: {value}\n\n")
}
fn imported(status: &str, delivery: Option<Value>) -> Value {
    let mut value = json!({"uploadId":42,"fightLogId":if status=="complete" { Some(99) } else { None },"status":status,"duplicate":false});
    if let Some(delivery) = delivery {
        value["discordDeliveryAccepted"] = json!(true);
        value["discordDelivery"] = delivery;
    }
    value
}
fn upload(
    replies: Vec<Result<HttpResponse, HttpError>>,
    route: DonBotDeliveryRoute,
) -> Result<DonBotUploadSuccess, DonBotError> {
    let fixture = Fixture::new();
    let (client, http) = setup(replies);
    client.upload(
        &fixture.identity(),
        &options(route),
        &key(),
        &http.cancel.token(),
    )
}
fn import(
    replies: Vec<Result<HttpResponse, HttpError>>,
    route: DonBotDeliveryRoute,
) -> Result<DonBotUploadSuccess, DonBotError> {
    let (client, http) = setup(replies);
    client.import_permalink(
        "https://dps.report/fixture",
        &options(route),
        &key(),
        &http.cancel.token(),
    )
}
fn header<'a>(seen: &'a Seen, name: &str) -> &'a [u8] {
    &seen
        .headers
        .iter()
        .find(|(n, _, _)| n.eq_ignore_ascii_case(name))
        .unwrap()
        .1
}
fn permanent(error: DonBotError) {
    assert_eq!(error.disposition, DonBotDisposition::Failed);
    assert!(error.retry_after.is_none());
    for private in ["SYNTHETIC", "private-response", "fixture.zevtc"] {
        assert!(!format!("{error}").contains(private));
    }
}

#[test]
fn tus_streams_the_exact_archive_to_each_allowed_location_and_retains_fight_id() {
    for location in [
        "file-123_abc",
        "/service/api/upload/tus/file-123_abc",
        "https://DONBOT.EXAMPLE/service/api/upload/tus/file-123_abc",
    ] {
        let fixture = Fixture::new();
        let (client, http) = setup(vec![
            create(location, true, false),
            patched(),
            reply(200, &completed(None)),
        ]);
        let result = client
            .upload(
                &fixture.identity(),
                &options(DonBotDeliveryRoute::None),
                &key(),
                &http.cancel.token(),
            )
            .unwrap();
        assert_eq!(result.upload_id, Some(42));
        assert_eq!(result.fight_log_id, Some(99));
        assert_eq!(
            result.discord_delivery,
            DonBotDiscordDeliveryReceipt::default()
        );
        let seen = http.seen.lock().unwrap();
        assert_eq!(seen.len(), 3);
        assert_eq!(seen[0].method, HttpMethod::Post);
        assert_eq!(seen[0].url, format!("{BASE}/api/upload/tus"));
        assert!(seen[0].body.is_empty());
        assert_eq!(
            header(&seen[0], "Upload-Metadata"),
            b"filename dXBsb2FkLnpldnRj,guildid MTIz,wingman ZmFsc2U="
        );
        assert_eq!(header(&seen[0], "Upload-Length"), b"9");
        assert_eq!(seen[1].method, HttpMethod::Patch);
        assert_eq!(seen[1].url, format!("{BASE}/api/upload/tus/file-123_abc"));
        assert_eq!(seen[1].body, PAYLOAD);
        assert_eq!(header(&seen[1], "Upload-Offset"), b"0");
        assert_eq!(
            header(&seen[1], "Content-Type"),
            b"application/offset+octet-stream"
        );
        assert_eq!(seen[2].method, HttpMethod::Get);
        assert_eq!(seen[2].url, format!("{BASE}/api/upload/stream/42"));
        assert_eq!(header(&seen[2], "Accept"), b"text/event-stream");
        assert!(seen[2].body.is_empty());
        for call in seen.iter() {
            assert_eq!(header(call, "X-GW2-API-Key"), b"SYNTHETIC-KEY");
            assert_eq!(
                call.headers
                    .iter()
                    .find(|(name, _, _)| name == "X-GW2-API-Key")
                    .unwrap()
                    .2,
                HttpHeaderSensitivity::Sensitive
            );
            assert!(!call.url.contains("SYNTHETIC"));
            assert_eq!(
                call.timeouts,
                HttpTimeouts {
                    connect: Duration::from_secs(10),
                    operation: Duration::from_secs(900),
                    stalled_transfer: Duration::from_secs(900)
                }
            );
            assert_eq!(call.limits.max_header_bytes, 65536);
        }
        assert_eq!(seen[0].limits.max_body_bytes, 65536);
        assert_eq!(seen[1].limits.max_body_bytes, 65536);
        assert_eq!(seen[2].limits.max_body_bytes, 262144);
    }
    let result = upload(
        vec![create("file", false, false), patched()],
        DonBotDeliveryRoute::None,
    )
    .unwrap();
    assert_eq!(result, DonBotUploadSuccess::default());
}

#[test]
fn location_attacks_and_bad_creation_handshakes_never_send_patch_or_credentials_elsewhere() {
    for location in [
        "https://evil.example/service/api/upload/tus/file",
        "https://donbot.example.evil/service/api/upload/tus/file",
        "https://user@donbot.example/service/api/upload/tus/file",
        "https://donbot.example:443/service/api/upload/tus/file",
        "//donbot.example/service/api/upload/tus/file",
        "/api/upload/tus/file",
        "/other/file",
        "../file",
        "file/extra",
        "file?secret=value",
        "file#x",
        "file%2fextra",
        "file\\x",
        "",
        "\u{96ea}",
    ] {
        let fixture = Fixture::new();
        let (client, http) = setup(vec![create(location, false, false)]);
        permanent(
            client
                .upload(
                    &fixture.identity(),
                    &options(DonBotDeliveryRoute::None),
                    &key(),
                    &http.cancel.token(),
                )
                .unwrap_err(),
        );
        assert_eq!(http.seen.lock().unwrap().len(), 1);
    }
    assert!(
        upload(
            vec![create(&"a".repeat(256), false, false), patched()],
            DonBotDeliveryRoute::None
        )
        .is_ok()
    );
    permanent(
        upload(
            vec![create(&"a".repeat(257), false, false)],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err(),
    );
    for mode in 0..8 {
        let mut r = create("file", true, false).unwrap();
        match mode {
            0 => r.headers.retain(|h| h.name() != "Location"),
            1 => r.headers.retain(|h| h.name() != "Tus-Resumable"),
            2 => r.headers.push(HttpHeader::public("location", "file")),
            3 => r.headers.push(HttpHeader::public("tus-resumable", "1.0.0")),
            4 => r.headers.push(HttpHeader::public("x-log-upload-id", "42")),
            5 => r.headers[1] = HttpHeader::public("Tus-Resumable", "0.2.2"),
            6 => r.headers[2] = HttpHeader::public("X-Log-Upload-Id", "0"),
            _ => r.headers[2] = HttpHeader::public("X-Log-Upload-Id", "9223372036854775808"),
        }
        permanent(upload(vec![Ok(r)], DonBotDeliveryRoute::None).unwrap_err());
    }
}

#[test]
fn creation_retries_but_every_uncertain_patch_settles_without_retry() {
    for status in [200, 202, 301, 400, 401, 403, 408, 429, 500, 599] {
        let error = upload(
            vec![reply(status, "private-response")],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err();
        let retry = status == 408 || status == 429 || status >= 500;
        assert_eq!(
            error.disposition,
            if retry {
                DonBotDisposition::Retry
            } else {
                DonBotDisposition::Failed
            }
        );
        assert_eq!(
            error.retry_after,
            retry.then_some(Duration::from_secs(if status == 429 { 60 } else { 30 }))
        );
        let error = upload(
            vec![
                create("file", false, false),
                reply(status, "private-response"),
            ],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err();
        assert_eq!(error.code, DonBotErrorCode::UnconfirmedUpload);
        permanent(error);
    }
    for code in [
        HttpErrorCode::Timeout,
        HttpErrorCode::NameResolutionFailed,
        HttpErrorCode::ConnectionFailed,
        HttpErrorCode::TlsFailed,
        HttpErrorCode::SendFailed,
        HttpErrorCode::ReceiveFailed,
        HttpErrorCode::InvalidRequest,
        HttpErrorCode::BodyReadFailed,
        HttpErrorCode::ProtocolError,
        HttpErrorCode::ResponseTooLarge,
        HttpErrorCode::Internal,
    ] {
        let error = upload(
            vec![create("file", false, false), Err(HttpError::new(code))],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err();
        assert_eq!(error.http_error, Some(code));
        permanent(error);
    }
    for mode in 0..8 {
        let mut r = patched().unwrap();
        match mode {
            0 => r.headers.clear(),
            1 => {
                r.headers.pop();
            }
            2 => r.headers.push(HttpHeader::public("Tus-Resumable", "1.0.0")),
            3 => r.headers.push(HttpHeader::public("upload-offset", "9")),
            4 => r.headers[0] = HttpHeader::public("Tus-Resumable", "1"),
            5 => r.headers[1] = HttpHeader::public("Upload-Offset", "8"),
            6 => r.headers[1] = HttpHeader::public("Upload-Offset", "09"),
            _ => r.headers[1] = HttpHeader::public("Upload-Offset", "+9"),
        };
        permanent(
            upload(
                vec![create("file", false, false), Ok(r)],
                DonBotDeliveryRoute::None,
            )
            .unwrap_err(),
        );
    }
}

#[test]
fn unavailable_optional_progress_preserves_upload_but_requested_delivery_requires_confirmation() {
    for requested in [false, true] {
        for transport in [false, true] {
            let route = if requested {
                DonBotDeliveryRoute::GuildDefaults
            } else {
                DonBotDeliveryRoute::None
            };
            let result = upload(
                vec![
                    create("file", true, requested),
                    patched(),
                    if transport {
                        Err(HttpError::new(HttpErrorCode::Timeout))
                    } else {
                        reply(503, "private-response")
                    },
                ],
                route,
            );
            if requested {
                let error = result.unwrap_err();
                assert_eq!(error.code, DonBotErrorCode::UnconfirmedDelivery);
                permanent(error);
            } else {
                let result = result.unwrap();
                assert_eq!(result.upload_id, Some(42));
                assert!(result.fight_log_id.is_none());
            }
        }
    }
    for requested in [false, true] {
        let result = upload(
            vec![
                create("file", true, requested),
                patched(),
                Err(HttpError::new(HttpErrorCode::Cancelled)),
            ],
            if requested {
                DonBotDeliveryRoute::GuildDefaults
            } else {
                DonBotDeliveryRoute::None
            },
        );
        assert_eq!(
            result.unwrap_err().disposition,
            DonBotDisposition::Cancelled
        );
    }
}

#[test]
fn completion_stream_requires_a_final_complete_event_and_normalized_matching_receipt() {
    for raw in [
        "",
        "data: {}",
        "data: []",
        "data: {\"stage\":1}",
        "data: {\"stage\":\"failed\"}",
        "data: {\"stage\":\"processing\"}",
        "data: {\"stage\":\"complete\",\"fightLogId\":0}",
        "data: {\"stage\":\"complete\",\"fightLogId\":9223372036854775808}",
        "data: {\"stage\":\"complete\",\"fightLogId\":1,\"fightLogId\":2}",
        "data: {\"stage\":\"complete\"}\ndata: {\"stage\":\"processing\"}",
        "data: {\"stage\":\"complete\",\"future\":\"\\uD800\"}",
    ] {
        permanent(
            upload(
                vec![create("file", true, false), patched(), reply(200, raw)],
                DonBotDeliveryRoute::None,
            )
            .unwrap_err(),
        );
    }
    let raw = ": heartbeat\r\nevent: progress\r\ndata: {\"stage\":\"processing\",\"fightLogId\":77}\r\n\r\ndata:{\"stage\":\"complete\",\"future\":1e400}\r\n\r\n";
    assert_eq!(
        upload(
            vec![create("file", true, false), patched(), reply(200, raw)],
            DonBotDeliveryRoute::None
        )
        .unwrap()
        .fight_log_id,
        Some(77)
    );
    assert!(
        upload(
            vec![
                create("file", true, false),
                patched(),
                reply(200, "data: {\"stage\":\"complete\"}")
            ],
            DonBotDeliveryRoute::None
        )
        .unwrap()
        .fight_log_id
        .is_none()
    );
    let oversized = format!("{}{}", completed(None), " ".repeat(256 * 1024));
    permanent(
        upload(
            vec![
                create("file", true, false),
                patched(),
                reply(200, &oversized),
            ],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err(),
    );
    let requested = receipt(true, "sent", [1, 0, 0, 0]);
    permanent(
        upload(
            vec![
                create("file", true, false),
                patched(),
                reply(200, &completed(Some(requested.clone()))),
            ],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err(),
    );
    permanent(
        upload(
            vec![
                create("file", true, true),
                patched(),
                reply(200, &completed(None)),
            ],
            DonBotDeliveryRoute::GuildDefaults,
        )
        .unwrap_err(),
    );
    let early = format!(
        "data: {{\"stage\":\"processing\",\"discordDelivery\":{requested}}}\n{}",
        completed(Some(requested.clone()))
    );
    permanent(
        upload(
            vec![create("file", true, true), patched(), reply(200, &early)],
            DonBotDeliveryRoute::GuildDefaults,
        )
        .unwrap_err(),
    );
    let final_missing = format!("{}{}", completed(Some(requested)), completed(None));
    permanent(
        upload(
            vec![
                create("file", true, true),
                patched(),
                reply(200, &final_missing),
            ],
            DonBotDeliveryRoute::GuildDefaults,
        )
        .unwrap_err(),
    );
}

#[test]
fn discord_routes_are_exactly_encoded_and_acknowledged_before_patch() {
    for (route, suffix) in [
        (
            DonBotDeliveryRoute::GuildDefaults,
            ",discorddelivery Z3VpbGRfZGVmYXVsdHM=",
        ),
        (
            DonBotDeliveryRoute::ChannelOverride {
                channel_id: "456".into(),
            },
            ",discorddelivery Y2hhbm5lbF9vdmVycmlkZQ==,discordchannelid NDU2",
        ),
    ] {
        let fixture = Fixture::new();
        let (client, http) = setup(vec![
            create("file", true, true),
            patched(),
            reply(
                200,
                &completed(Some(receipt(true, "partial", [2, 1, 0, 0]))),
            ),
        ]);
        let result = client
            .upload(
                &fixture.identity(),
                &options(route.clone()),
                &key(),
                &http.cancel.token(),
            )
            .unwrap();
        assert_eq!(
            result.discord_delivery.outcome,
            DonBotDiscordDeliveryOutcome::Partial
        );
        assert_eq!(result.discord_delivery.sent, 2);
        assert_eq!(
            header(&http.seen.lock().unwrap()[0], "Upload-Metadata"),
            format!("filename dXBsb2FkLnpldnRj,guildid MTIz,wingman ZmFsc2U={suffix}").as_bytes()
        );
        for mode in 0..4 {
            let mut r = create("file", mode != 0, mode != 1).unwrap();
            if mode == 2 {
                r.headers
                    .push(HttpHeader::public("X-DonBot-Discord-Delivery", "accepted"));
            }
            if mode == 3 {
                *r.headers.last_mut().unwrap() =
                    HttpHeader::public("X-DonBot-Discord-Delivery", "rejected");
            }
            permanent(upload(vec![Ok(r)], route.clone()).unwrap_err());
        }
    }
    for (id, encoded) in [
        ("1", "MQ=="),
        ("12", "MTI="),
        ("123", "MTIz"),
        ("1234", "MTIzNA=="),
        ("9223372036854775807", "OTIyMzM3MjAzNjg1NDc3NTgwNw=="),
    ] {
        let fixture = Fixture::new();
        let (client, http) = setup(vec![create("file", false, false), patched()]);
        let mut options = options(DonBotDeliveryRoute::None);
        options.guild_id = id.into();
        client
            .upload(&fixture.identity(), &options, &key(), &http.cancel.token())
            .unwrap();
        assert_eq!(
            header(&http.seen.lock().unwrap()[0], "Upload-Metadata"),
            format!("filename dXBsb2FkLnpldnRj,guildid {encoded},wingman ZmFsc2U=").as_bytes()
        );
    }
}

#[test]
fn permalink_import_is_idempotent_and_never_reads_the_archive() {
    for (status, duplicate) in [(200, true), (202, false)] {
        for origin in ["dps.report", "b.dps.report", "wvw.report"] {
            let mut body = imported("complete", None);
            body["duplicate"] = json!(duplicate);
            let (client, http) = setup(vec![reply(status, &body.to_string())]);
            let url = format!("https://{origin}/fixture");
            let result = client
                .import_permalink(
                    &url,
                    &options(DonBotDeliveryRoute::None),
                    &key(),
                    &http.cancel.token(),
                )
                .unwrap();
            assert_eq!(
                (result.upload_id, result.fight_log_id),
                (Some(42), Some(99))
            );
            let seen = http.seen.lock().unwrap();
            assert_eq!(seen.len(), 1);
            assert_eq!(seen[0].url, format!("{BASE}/api/upload/gw2/url"));
            assert_eq!(
                seen[0].body,
                format!("{{\"url\":\"{url}\",\"guildId\":\"123\"}}").as_bytes()
            );
            assert_eq!(header(&seen[0], "X-GW2-API-Key"), b"SYNTHETIC-KEY");
        }
    }
    let result = import(
        vec![
            reply(202, &imported("pending", None).to_string()),
            reply(200, &completed(None)),
        ],
        DonBotDeliveryRoute::None,
    )
    .unwrap();
    assert_eq!(result.fight_log_id, Some(99));
    for route in [
        DonBotDeliveryRoute::GuildDefaults,
        DonBotDeliveryRoute::ChannelOverride {
            channel_id: "456".into(),
        },
    ] {
        let wire = if matches!(route, DonBotDeliveryRoute::GuildDefaults) {
            json!({"mode":"guild_defaults"})
        } else {
            json!({"mode":"channel_override","channelId":"456"})
        };
        let (client, http) = setup(vec![reply(
            200,
            &imported("complete", Some(receipt(true, "sent", [1, 0, 0, 0]))).to_string(),
        )]);
        client
            .import_permalink(
                "https://dps.report/fixture",
                &options(route),
                &key(),
                &http.cancel.token(),
            )
            .unwrap();
        assert_eq!(
            serde_json::from_slice::<Value>(&http.seen.lock().unwrap()[0].body).unwrap(),
            json!({"url":"https://dps.report/fixture","guildId":"123","discordDelivery":wire})
        );
    }
}

#[test]
fn import_shape_ids_acknowledgement_and_request_intent_are_validated() {
    for (field, value) in [
        ("uploadId", json!(0)),
        ("uploadId", json!(9223372036854775808u64)),
        ("uploadId", json!(1.0)),
        ("fightLogId", json!(0)),
        ("fightLogId", json!(null)),
        ("status", json!("pending")),
        ("status", json!("failed")),
        ("duplicate", json!(1)),
    ] {
        let mut body = imported("complete", None);
        body[field] = value;
        permanent(
            import(
                vec![reply(200, &body.to_string())],
                DonBotDeliveryRoute::None,
            )
            .unwrap_err(),
        );
    }
    for field in ["uploadId", "status", "duplicate"] {
        let mut body = imported("complete", None);
        body.as_object_mut().unwrap().remove(field);
        permanent(
            import(
                vec![reply(200, &body.to_string())],
                DonBotDeliveryRoute::None,
            )
            .unwrap_err(),
        );
    }
    for body in [
        "[]",
        "{}",
        "garbage",
        r#"{"uploadId":42,"status":"pending","duplicate":false,"unknown":"\uD800"}"#,
    ] {
        permanent(import(vec![reply(200, body)], DonBotDeliveryRoute::None).unwrap_err());
    }
    permanent(
        import(
            vec![reply(200, &" ".repeat(65537))],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err(),
    );
    let body = imported("complete", Some(receipt(true, "sent", [1, 0, 0, 0])));
    permanent(
        import(
            vec![reply(200, &body.to_string())],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err(),
    );
    permanent(
        import(
            vec![reply(200, &imported("complete", None).to_string())],
            DonBotDeliveryRoute::GuildDefaults,
        )
        .unwrap_err(),
    );
    for status in ["complete", "pending"] {
        let mut body = imported(status, Some(receipt(true, "sent", [1, 0, 0, 0])));
        body["discordDeliveryAccepted"] = json!(false);
        permanent(
            import(
                vec![reply(200, &body.to_string())],
                DonBotDeliveryRoute::GuildDefaults,
            )
            .unwrap_err(),
        );
    }
}

#[test]
fn normalized_receipt_outcomes_and_counts_match_the_request() {
    for (outcome, counts) in [
        ("sent", [4, 0, 0, 0]),
        ("partial", [1, 1, 1, 1]),
        ("skipped", [0, 4, 0, 0]),
        ("failed", [0, 0, 4, 0]),
        ("ambiguous", [0, 0, 0, 4]),
    ] {
        let value = receipt(true, outcome, counts);
        let result = import(
            vec![reply(
                200,
                &imported("complete", Some(value.clone())).to_string(),
            )],
            DonBotDeliveryRoute::GuildDefaults,
        )
        .unwrap();
        assert_eq!(
            [
                result.discord_delivery.sent,
                result.discord_delivery.skipped,
                result.discord_delivery.failed,
                result.discord_delivery.ambiguous
            ],
            counts
        );
        assert!(
            upload(
                vec![
                    create("file", true, true),
                    patched(),
                    reply(200, &completed(Some(value)))
                ],
                DonBotDeliveryRoute::GuildDefaults
            )
            .is_ok()
        );
    }
    for value in [
        receipt(true, "not_requested", [0, 0, 0, 0]),
        receipt(false, "sent", [1, 0, 0, 0]),
        receipt(true, "unknown", [1, 0, 0, 0]),
        receipt(true, "sent", [0, 0, 0, 0]),
        receipt(true, "sent", [1, 1, 0, 0]),
        receipt(true, "partial", [1, 0, 0, 0]),
        receipt(true, "partial", [4, 1, 0, 0]),
        receipt(true, "failed", [1, 0, 0, 0]),
        receipt(true, "ambiguous", [0, 0, 0, 65535]),
    ] {
        permanent(
            import(
                vec![reply(200, &imported("complete", Some(value)).to_string())],
                DonBotDeliveryRoute::GuildDefaults,
            )
            .unwrap_err(),
        );
    }
    for field in [
        "requested",
        "outcome",
        "sent",
        "skipped",
        "failed",
        "ambiguous",
    ] {
        let mut value = receipt(true, "sent", [1, 0, 0, 0]);
        value.as_object_mut().unwrap().remove(field);
        permanent(
            import(
                vec![reply(200, &imported("complete", Some(value)).to_string())],
                DonBotDeliveryRoute::GuildDefaults,
            )
            .unwrap_err(),
        );
    }
    assert!(
        import(
            vec![reply(
                200,
                &imported(
                    "complete",
                    Some(receipt(false, "not_requested", [0, 0, 0, 0]))
                )
                .to_string()
            )],
            DonBotDeliveryRoute::None
        )
        .is_ok()
    );
}

#[test]
fn aggregate_requests_keep_string_ids_and_bounded_delivery_results() {
    for count in [2, 100] {
        let ids: Vec<_> = (1..=count).collect();
        let body =
            json!({"fightLogCount":count,"discordDelivery":receipt(true,"sent",[100,0,0,0])});
        let (client, http) = setup(vec![reply(200, &body.to_string())]);
        let result = client
            .deliver_aggregate(
                &ids,
                &options(DonBotDeliveryRoute::GuildDefaults),
                &key(),
                &http.cancel.token(),
            )
            .unwrap();
        assert_eq!(result.fight_log_count, count as usize);
        assert_eq!(result.discord_delivery.sent, 100);
        let seen = http.seen.lock().unwrap();
        assert_eq!(seen.len(), 1);
        assert_eq!(seen[0].url, format!("{BASE}/api/upload/gw2/aggregate"));
        assert_eq!(
            serde_json::from_slice::<Value>(&seen[0].body).unwrap(),
            json!({"guildId":"123","fightLogIds":ids.iter().map(u64::to_string).collect::<Vec<_>>(),"discordDelivery":{"mode":"guild_defaults"}})
        );
    }
    for body in [
        json!({}),
        json!({"fightLogCount":3,"discordDelivery":receipt(true,"sent",[1,0,0,0])}),
        json!({"fightLogCount":2,"discordDelivery":receipt(true,"sent",[101,0,0,0])}),
        json!({"fightLogCount":2,"discordDelivery":receipt(false,"not_requested",[0,0,0,0])}),
    ] {
        let (client, http) = setup(vec![reply(200, &body.to_string())]);
        permanent(
            client
                .deliver_aggregate(
                    &[1, 2],
                    &options(DonBotDeliveryRoute::GuildDefaults),
                    &key(),
                    &http.cancel.token(),
                )
                .unwrap_err(),
        );
        assert_eq!(http.seen.lock().unwrap().len(), 1);
    }
}

#[test]
fn invalid_options_links_ids_and_sizes_fail_before_any_request() {
    let fixture = Fixture::new();
    let (client, http) = setup(vec![]);
    let stop = http.cancel.token();
    for mode in 0..5 {
        let mut options = options(DonBotDeliveryRoute::None);
        match mode {
            0 => options.api_base_url = "http://donbot.example".into(),
            1 => options.guild_id.clear(),
            2 => options.guild_id = "01".into(),
            3 => options.guild_id = "9223372036854775808".into(),
            _ => {
                options.delivery = DonBotDeliveryRoute::ChannelOverride {
                    channel_id: "0".into(),
                }
            }
        };
        permanent(
            client
                .upload(&fixture.identity(), &options, &key(), &stop)
                .unwrap_err(),
        );
    }
    for size in [0, MAX_HTTP_REQUEST_BODY_BYTES + 1] {
        let mut file = fixture.identity();
        file.size = size;
        permanent(
            client
                .upload(&file, &options(DonBotDeliveryRoute::None), &key(), &stop)
                .unwrap_err(),
        );
    }
    permanent(
        client
            .upload(
                &fixture.identity(),
                &options(DonBotDeliveryRoute::None),
                &SecretValue::default(),
                &stop,
            )
            .unwrap_err(),
    );
    for url in [
        "https://evil.example/a",
        "http://dps.report/a",
        "https://dps.report/",
        "https://dps.report/a?secret=x",
        "https://dps.report/a#x",
        "https://dps.report/a@b",
        "https://dps.report/a\\b",
    ] {
        permanent(
            client
                .import_permalink(url, &options(DonBotDeliveryRoute::None), &key(), &stop)
                .unwrap_err(),
        );
    }
    for ids in [
        vec![],
        vec![1],
        vec![1, 1],
        vec![0, 2],
        vec![1, i64::MAX as u64 + 1],
        (1..=101).collect(),
    ] {
        permanent(
            client
                .deliver_aggregate(
                    &ids,
                    &options(DonBotDeliveryRoute::GuildDefaults),
                    &key(),
                    &stop,
                )
                .unwrap_err(),
        );
    }
    permanent(
        client
            .deliver_aggregate(&[1, 2], &options(DonBotDeliveryRoute::None), &key(), &stop)
            .unwrap_err(),
    );
    assert!(http.seen.lock().unwrap().is_empty());
}

#[test]
fn file_identity_is_opened_after_creation_and_rechecked_during_patch() {
    let fixture = Fixture::new();
    let mut file = fixture.identity();
    file.size += 1;
    let (client, http) = setup(vec![create("file", false, false)]);
    assert_eq!(
        client
            .upload(
                &file,
                &options(DonBotDeliveryRoute::None),
                &key(),
                &http.cancel.token()
            )
            .unwrap_err()
            .code,
        DonBotErrorCode::FileUnavailable
    );
    assert_eq!(http.seen.lock().unwrap().len(), 1);
    let file = fixture.identity();
    let (client, http) = configured(
        vec![create("file", false, false), patched()],
        None,
        None,
        Some((1, fixture.0.clone())),
    );
    let error = client
        .upload(
            &file,
            &options(DonBotDeliveryRoute::None),
            &key(),
            &http.cancel.token(),
        )
        .unwrap_err();
    assert_eq!(error.http_error, Some(HttpErrorCode::BodyReadFailed));
    permanent(error);
}

#[test]
fn cancellation_between_protocol_phases_and_panics_stop_followup_requests() {
    for phase in 0..3 {
        let fixture = Fixture::new();
        let (client, http) = configured(
            vec![
                create("file", true, false),
                patched(),
                reply(200, &completed(None)),
            ],
            Some(phase),
            None,
            None,
        );
        assert_eq!(
            client
                .upload(
                    &fixture.identity(),
                    &options(DonBotDeliveryRoute::None),
                    &key(),
                    &http.cancel.token()
                )
                .unwrap_err()
                .disposition,
            DonBotDisposition::Cancelled
        );
        assert_eq!(http.seen.lock().unwrap().len(), phase + 1);
        assert_eq!(
            client
                .import_permalink(
                    "https://dps.report/fixture",
                    &options(DonBotDeliveryRoute::None),
                    &key(),
                    &http.cancel.token()
                )
                .unwrap_err()
                .disposition,
            DonBotDisposition::Cancelled
        );
        assert_eq!(http.seen.lock().unwrap().len(), phase + 1);
    }
    for phase in 0..3 {
        let fixture = Fixture::new();
        let (client, http) = configured(
            vec![
                create("file", true, false),
                patched(),
                reply(200, &completed(None)),
            ],
            None,
            Some(phase),
            None,
        );
        permanent(
            client
                .upload(
                    &fixture.identity(),
                    &options(DonBotDeliveryRoute::None),
                    &key(),
                    &http.cancel.token(),
                )
                .unwrap_err(),
        );
    }
}

#[test]
fn import_and_aggregate_failures_make_one_request_and_preserve_typed_categories() {
    for aggregate in [false, true] {
        for status in [201, 204, 301, 400, 401, 403, 408, 429, 500] {
            let (client, http) = setup(vec![reply(status, "private-response")]);
            let error = if aggregate {
                client
                    .deliver_aggregate(
                        &[1, 2],
                        &options(DonBotDeliveryRoute::GuildDefaults),
                        &key(),
                        &http.cancel.token(),
                    )
                    .unwrap_err()
            } else {
                client
                    .import_permalink(
                        "https://dps.report/fixture",
                        &options(DonBotDeliveryRoute::None),
                        &key(),
                        &http.cancel.token(),
                    )
                    .unwrap_err()
            };
            let retry = matches!(status, 408 | 429 | 500);
            assert_eq!(
                error.disposition,
                if retry {
                    DonBotDisposition::Retry
                } else {
                    DonBotDisposition::Failed
                }
            );
            assert_eq!(error.http_status, Some(status));
            assert_eq!(http.seen.lock().unwrap().len(), 1);
        }
        for code in [
            HttpErrorCode::Timeout,
            HttpErrorCode::Cancelled,
            HttpErrorCode::ProtocolError,
        ] {
            let (client, http) = setup(vec![Err(HttpError::new(code))]);
            let error = if aggregate {
                client
                    .deliver_aggregate(
                        &[1, 2],
                        &options(DonBotDeliveryRoute::GuildDefaults),
                        &key(),
                        &http.cancel.token(),
                    )
                    .unwrap_err()
            } else {
                client
                    .import_permalink(
                        "https://dps.report/fixture",
                        &options(DonBotDeliveryRoute::None),
                        &key(),
                        &http.cancel.token(),
                    )
                    .unwrap_err()
            };
            assert_eq!(error.http_error, Some(code));
            assert_eq!(
                error.disposition,
                match code {
                    HttpErrorCode::Timeout => DonBotDisposition::Retry,
                    HttpErrorCode::Cancelled => DonBotDisposition::Cancelled,
                    _ => DonBotDisposition::Failed,
                }
            );
            assert_eq!(http.seen.lock().unwrap().len(), 1);
        }
    }
}

#[test]
fn pending_import_receipts_cannot_contradict_captured_delivery_intent() {
    let mut body = imported(
        "pending",
        Some(receipt(false, "not_requested", [0, 0, 0, 0])),
    );
    body["discordDeliveryAccepted"] = json!(true);
    permanent(
        import(
            vec![reply(202, &body.to_string())],
            DonBotDeliveryRoute::GuildDefaults,
        )
        .unwrap_err(),
    );
    let body = imported("pending", Some(receipt(true, "sent", [1, 0, 0, 0])));
    permanent(
        import(
            vec![reply(202, &body.to_string())],
            DonBotDeliveryRoute::None,
        )
        .unwrap_err(),
    );
    let mut body = imported("pending", None);
    body["discordDeliveryAccepted"] = json!(true);
    let result = import(
        vec![
            reply(202, &body.to_string()),
            reply(200, &completed(Some(receipt(true, "sent", [1, 0, 0, 0])))),
        ],
        DonBotDeliveryRoute::GuildDefaults,
    )
    .unwrap();
    assert_eq!(result.discord_delivery.sent, 1);
}

#[test]
fn aggregate_override_and_cancellation_after_dispatch_preserve_request_boundaries() {
    let (client, http) = setup(vec![reply(
        200,
        &json!({"fightLogCount":2,"discordDelivery":receipt(true,"ambiguous",[0,0,0,2])})
            .to_string(),
    )]);
    let result = client
        .deliver_aggregate(
            &[1, 2],
            &options(DonBotDeliveryRoute::ChannelOverride {
                channel_id: "456".into(),
            }),
            &key(),
            &http.cancel.token(),
        )
        .unwrap();
    assert_eq!(
        result.discord_delivery.outcome,
        DonBotDiscordDeliveryOutcome::Ambiguous
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&http.seen.lock().unwrap()[0].body).unwrap()["discordDelivery"],
        json!({"mode":"channel_override","channelId":"456"})
    );
    let (client, http) = configured(
        vec![reply(
            200,
            &json!({"fightLogCount":2,"discordDelivery":receipt(true,"sent",[1,0,0,0])})
                .to_string(),
        )],
        Some(0),
        None,
        None,
    );
    assert_eq!(
        client
            .deliver_aggregate(
                &[1, 2],
                &options(DonBotDeliveryRoute::GuildDefaults),
                &key(),
                &http.cancel.token()
            )
            .unwrap_err()
            .disposition,
        DonBotDisposition::Cancelled
    );
    assert_eq!(http.seen.lock().unwrap().len(), 1);
    assert_eq!(
        client
            .deliver_aggregate(
                &[1, 2],
                &options(DonBotDeliveryRoute::GuildDefaults),
                &key(),
                &http.cancel.token()
            )
            .unwrap_err()
            .disposition,
        DonBotDisposition::Cancelled
    );
    assert_eq!(http.seen.lock().unwrap().len(), 1);
}
