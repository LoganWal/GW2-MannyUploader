use manny_uploader::application::*;
use manny_uploader::providers::*;
use serde_json::{Value, json};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const BASE: &str = "https://donbot.example/service";
const SUMMARY: &str = "discord-summary-delivery-v1";
const AGGREGATE: &str = "discord-aggregate-delivery-v1";
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
) -> DonBotClient {
    DonBotClient::new(Arc::new(Http(f)))
}
fn key() -> SecretValue {
    SecretValue::from_bytes(b"SYNTHETIC-Key-123".to_vec())
}
fn response(status: u16, bytes: Vec<u8>) -> HttpResponse {
    HttpResponse {
        status_code: status,
        headers: vec![],
        body: SecretValue::from_bytes(bytes),
    }
}
fn guild(id: usize) -> Value {
    json!({"guildId":id.to_string(),"guildName":"Guild"})
}
fn channel(id: usize) -> Value {
    json!({"channelId":id.to_string(),"channelName":"Channel"})
}
fn plain() -> Value {
    json!({"accountName":"Player.1234","guilds":[guild(1)]})
}
fn delivery() -> Value {
    let mut value = plain();
    value["capabilities"] = json!([SUMMARY, AGGREGATE, "future-capability"]);
    value["guilds"][0]["discordDelivery"] = json!({"enabled":true,"defaultsAvailable":true,"channelOverrideAllowed":true,"enabledMessageKinds":["pve-summary","wvw-summary","wvw-advanced","wvw-stream"],"channels":[channel(2)],"aggregateEnabled":true,"aggregateDefaultsAvailable":true,"maxAggregateFightLogs":50});
    value
}
fn bytes(bytes: Vec<u8>) -> Result<DonBotVerification, DonBotError> {
    client(move |_, _| Ok(response(200, bytes.clone()))).verify(
        BASE,
        &key(),
        &CancellationSource::default().token(),
    )
}
fn decode(value: Value) -> Result<DonBotVerification, DonBotError> {
    bytes(serde_json::to_vec(&value).unwrap())
}
fn invalid(value: Value) {
    let error = decode(value).unwrap_err();
    assert_eq!(error.code, DonBotErrorCode::InvalidResponse);
    assert_eq!(error.disposition, DonBotDisposition::Failed);
    for private in ["Player.1234", "Guild", "Channel", "SYNTHETIC"] {
        assert!(!format!("{error}").contains(private));
    }
}

#[test]
fn verification_streams_only_a_wiping_json_key_with_discovery_timeouts() {
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let client = client(move |mut request, stop| {
        count.fetch_add(1, Ordering::Relaxed);
        validate_http_request(&request, HttpTransportPolicy::HttpsOnly).unwrap();
        assert_eq!(request.method, HttpMethod::Post);
        assert_eq!(request.url, format!("{BASE}/api/upload/gw2/guilds"));
        assert_eq!(
            request.timeouts,
            HttpTimeouts {
                connect: Duration::from_secs(5),
                operation: Duration::from_secs(30),
                stalled_transfer: Duration::from_secs(30)
            }
        );
        assert_eq!(
            request.response_limits,
            HttpResponseLimits {
                max_header_bytes: 65536,
                max_body_bytes: 262144
            }
        );
        assert_eq!(request.headers.len(), 2);
        assert!(request.headers.iter().all(|h| h.name() != "X-GW2-API-Key"));
        assert!(
            request
                .headers
                .iter()
                .any(|h| h.name() == "Accept" && h.expose_value() == b"application/json")
        );
        assert!(
            request
                .headers
                .iter()
                .any(|h| h.name() == "Content-Type" && h.expose_value() == b"application/json")
        );
        assert!(!format!("{request:?}").contains("SYNTHETIC"));
        let source = request.body.as_mut().unwrap();
        let mut actual = Vec::new();
        let mut part = [0; 3];
        loop {
            let n = source.read(&mut part, stop).unwrap();
            if n == 0 {
                break;
            }
            actual.extend_from_slice(&part[..n]);
        }
        assert_eq!(actual.len() as u64, source.content_length());
        assert_eq!(actual, b"{\"apiKey\":\"SYNTHETIC-Key-123\"}");
        Ok(response(200, serde_json::to_vec(&plain()).unwrap()))
    });
    let secret = key();
    let identity = client
        .verify(
            &format!("{BASE}///"),
            &secret,
            &CancellationSource::default().token(),
        )
        .unwrap();
    assert_eq!(identity.account_name, "Player.1234");
    assert_eq!(identity.guilds[0].guild_id, "1");
    assert!(!identity.discord_summary_delivery_v1);
    assert_eq!(
        identity.guilds[0].discord_delivery,
        DonBotDiscordDeliveryPolicy::default()
    );
    assert_eq!(calls.load(Ordering::Relaxed), 1);
    assert!(secret.expose() == b"SYNTHETIC-Key-123");
}

#[test]
fn capabilities_gate_summary_and_aggregate_policy_without_inventing_authorization() {
    let identity = decode(delivery()).unwrap();
    assert!(identity.discord_summary_delivery_v1 && identity.discord_aggregate_delivery_v1);
    let policy = &identity.guilds[0].discord_delivery;
    assert!(policy.enabled && policy.defaults_available && policy.channel_override_allowed);
    assert!(policy.pve_summary && policy.wvw_summary && policy.wvw_advanced && policy.wvw_stream);
    assert!(policy.aggregate_enabled && policy.aggregate_defaults_available);
    assert_eq!(policy.max_aggregate_fight_logs, 50);
    assert_eq!(policy.channels[0].channel_id, "2");
    let mut value = delivery();
    value.as_object_mut().unwrap().remove("capabilities");
    let identity = decode(value).unwrap();
    assert_eq!(
        identity.guilds[0].discord_delivery,
        DonBotDiscordDeliveryPolicy::default()
    );
    let mut value = delivery();
    value["capabilities"] = json!([SUMMARY]);
    let policy = decode(value).unwrap().guilds.remove(0).discord_delivery;
    assert!(policy.enabled);
    assert!(!policy.aggregate_enabled);
    assert_eq!(policy.max_aggregate_fight_logs, 0);
    let mut value = plain();
    value["capabilities"] = json!([AGGREGATE]);
    invalid(value);
    let mut value = delivery();
    value["guilds"][0]["discordDelivery"]["enabledMessageKinds"] = json!([]);
    assert!(decode(value).is_ok());
    let mut value = plain();
    value["guilds"] = json!([]);
    assert!(decode(value).unwrap().guilds.is_empty());
}

#[test]
fn names_and_identifiers_require_bounded_utf8_and_canonical_positive_i64_values() {
    for pointer in [
        "/accountName",
        "/guilds/0/guildName",
        "/guilds/0/discordDelivery/channels/0/channelName",
    ] {
        for text in [
            "".into(),
            "x".repeat(257),
            "x\ny".into(),
            "x\u{7f}y".into(),
            "x\0y".into(),
            "\u{96ea}".repeat(86),
        ] {
            let mut value = delivery();
            *value.pointer_mut(pointer).unwrap() = json!(text);
            invalid(value);
        }
        for text in ["x".repeat(256), "\u{96ea}".repeat(85)] {
            let mut value = delivery();
            *value.pointer_mut(pointer).unwrap() = json!(text);
            assert!(decode(value).is_ok());
        }
    }
    for pointer in [
        "/guilds/0/guildId",
        "/guilds/0/discordDelivery/channels/0/channelId",
    ] {
        for id in [
            "",
            "0",
            "01",
            "-1",
            "+1",
            "1.0",
            "1e1",
            "9223372036854775808",
            "18446744073709551615",
            "\u{ff11}",
            "1 ",
        ] {
            let mut value = delivery();
            *value.pointer_mut(pointer).unwrap() = json!(id);
            invalid(value);
        }
        for id in ["1", "9223372036854775807"] {
            let mut value = delivery();
            *value.pointer_mut(pointer).unwrap() = json!(id);
            assert!(decode(value).is_ok());
        }
    }
    let mut value = plain();
    value["guilds"] = json!([guild(1), guild(1)]);
    invalid(value);
    let mut value = delivery();
    value["guilds"][0]["discordDelivery"]["channels"] = json!([channel(2), channel(2)]);
    invalid(value);
    let mut value = delivery();
    let second = value["guilds"][0].clone();
    value["guilds"].as_array_mut().unwrap().push(second);
    value["guilds"][1]["guildId"] = json!("2");
    assert!(decode(value).is_ok());
}

#[test]
fn required_fields_types_and_known_duplicates_are_rejected() {
    for field in ["accountName", "guilds"] {
        let mut value = plain();
        value.as_object_mut().unwrap().remove(field);
        invalid(value);
    }
    for field in ["guildId", "guildName"] {
        let mut value = plain();
        value["guilds"][0].as_object_mut().unwrap().remove(field);
        invalid(value);
    }
    for field in [
        "enabled",
        "defaultsAvailable",
        "channelOverrideAllowed",
        "enabledMessageKinds",
        "channels",
        "aggregateEnabled",
        "aggregateDefaultsAvailable",
        "maxAggregateFightLogs",
    ] {
        let mut value = delivery();
        value["guilds"][0]["discordDelivery"]
            .as_object_mut()
            .unwrap()
            .remove(field);
        invalid(value);
    }
    for field in ["channelId", "channelName"] {
        let mut value = delivery();
        value["guilds"][0]["discordDelivery"]["channels"][0]
            .as_object_mut()
            .unwrap()
            .remove(field);
        invalid(value);
    }
    for (pointer, replacement) in [
        ("/accountName", json!(1)),
        ("/guilds", json!({})),
        ("/guilds/0", json!([])),
        ("/guilds/0/guildId", json!(1)),
        ("/capabilities", json!([1])),
        ("/guilds/0/discordDelivery", json!([])),
        ("/guilds/0/discordDelivery/enabled", json!(1)),
        ("/guilds/0/discordDelivery/channels", json!({})),
        ("/guilds/0/discordDelivery/channels/0", json!([])),
    ] {
        let mut value = delivery();
        *value.pointer_mut(pointer).unwrap() = replacement;
        invalid(value);
    }
    for raw in [
        r#"{"accountName":"a","accountName":"b","guilds":[]}"#,
        r#"{"accountName":"a","guilds":[{"guildId":"1","guildId":"2","guildName":"g"}]}"#,
        r#"{"accountName":"a","guilds":[],"guilds":[]}"#,
    ] {
        assert!(bytes(raw.as_bytes().to_vec()).is_err());
    }
}

#[test]
fn message_kinds_and_aggregate_limits_are_exact_and_unique() {
    for kinds in [
        json!(["unknown"]),
        json!(["pve-summary", "pve-summary"]),
        json!([
            "pve-summary",
            "wvw-summary",
            "wvw-advanced",
            "wvw-stream",
            "unknown"
        ]),
        json!([1]),
    ] {
        let mut value = delivery();
        value["guilds"][0]["discordDelivery"]["enabledMessageKinds"] = kinds;
        invalid(value);
    }
    for bound in [
        json!(0),
        json!(1),
        json!(101),
        json!(-1),
        json!(2.0),
        json!("2"),
        json!(null),
        json!(65536),
    ] {
        let mut value = delivery();
        value["guilds"][0]["discordDelivery"]["maxAggregateFightLogs"] = bound;
        invalid(value);
    }
    for bound in [2, 100] {
        let mut value = delivery();
        value["guilds"][0]["discordDelivery"]["maxAggregateFightLogs"] = json!(bound);
        assert!(decode(value).is_ok());
    }
}

#[test]
fn guild_channel_and_document_budgets_are_enforced_at_boundaries() {
    for (count, valid) in [(256, true), (257, false)] {
        let mut value = plain();
        value["guilds"] = json!((1..=count).map(guild).collect::<Vec<_>>());
        assert_eq!(decode(value).is_ok(), valid);
        let mut value = delivery();
        value["guilds"][0]["discordDelivery"]["channels"] =
            json!((1..=count).map(channel).collect::<Vec<_>>());
        assert_eq!(decode(value).is_ok(), valid);
    }
    for (extra, valid) in [(0, true), (1, false)] {
        let mut value = delivery();
        let mut guilds = Vec::new();
        for id in 1..=9 {
            let mut guild = value["guilds"][0].clone();
            guild["guildId"] = json!(id.to_string());
            guild["discordDelivery"]["channels"] = json!(
                (1..=if id == 9 { extra } else { 256 })
                    .map(channel)
                    .collect::<Vec<_>>()
            );
            guilds.push(guild);
        }
        value["guilds"] = json!(guilds);
        assert!(serde_json::to_vec(&value).unwrap().len() < MAX_DONBOT_VERIFICATION_BYTES);
        assert_eq!(decode(value).is_ok(), valid);
    }
    let mut document = serde_json::to_vec(&plain()).unwrap();
    document.resize(MAX_DONBOT_VERIFICATION_BYTES, b' ');
    assert!(bytes(document.clone()).is_ok());
    document.push(b' ');
    assert!(bytes(document).is_err());
}

#[test]
fn unknown_fields_remain_forward_compatible_without_skipping_unicode_validation() {
    assert!(
        bytes(
            br#"{"accountName":"a","guilds":[],"future":{"value":1e400,"text":"\uD800\uDC00"}}"#
                .to_vec()
        )
        .is_ok()
    );
    for raw in [
        br#"{"accountName":"a","guilds":[],"future":"\uD800"}"#.as_slice(),
        b"[]",
        b"{}",
        b"not-json",
        b"{\"accountName\":\"\xff\",\"guilds\":[]}",
        br#"{"accountName":"a","guilds":[]}false"#,
    ] {
        assert!(bytes(raw.to_vec()).is_err());
    }
}

#[test]
fn invalid_keys_and_base_urls_fail_before_network_and_never_enter_diagnostics() {
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let client = client(move |_, _| {
        count.fetch_add(1, Ordering::Relaxed);
        Ok(response(200, serde_json::to_vec(&plain()).unwrap()))
    });
    let stop = CancellationSource::default().token();
    for url in [
        "",
        "http://donbot.example",
        "https:///path",
        "https://user@donbot.example",
        "https://donbot.example?key=SYNTHETIC",
        "https://donbot.example#x",
        "https://donbot.example/a/../b",
        "https://donbot.example/.",
        "https://donbot.example/a\\b",
        "https://donbot.example/\u{96ea}",
        "https://donbot.example:0",
        "https://donbot.example:invalid",
        "https://.example",
        "https://donbot.example.",
    ] {
        let error = client.verify(url, &key(), &stop).unwrap_err();
        assert_eq!(error.code, DonBotErrorCode::InvalidBase);
        assert!(!format!("{error:?}").contains("SYNTHETIC"));
    }
    assert_eq!(
        client
            .verify(
                &format!("https://donbot.example/{}", "a".repeat(2048)),
                &key(),
                &stop
            )
            .unwrap_err()
            .code,
        DonBotErrorCode::InvalidBase
    );
    for bytes in [
        vec![],
        vec![b'A'; 513],
        b"key secret".to_vec(),
        b"key\"secret".to_vec(),
        b"key\nsecret".to_vec(),
        vec![255],
        b"key_1".to_vec(),
    ] {
        assert_eq!(
            client
                .verify(BASE, &SecretValue::from_bytes(bytes), &stop)
                .unwrap_err()
                .code,
            DonBotErrorCode::InvalidKey
        );
    }
    assert_eq!(calls.load(Ordering::Relaxed), 0);
    assert!(
        client
            .verify(
                "https://[::1]:443/service///",
                &SecretValue::from_bytes(vec![b'A'; 512]),
                &stop
            )
            .is_ok()
    );
}

#[test]
fn statuses_and_transport_errors_preserve_safe_retry_classification() {
    for status in [
        201, 202, 204, 301, 400, 401, 403, 404, 408, 409, 429, 500, 599,
    ] {
        let client = client(move |_, _| Ok(response(status, b"private-response".to_vec())));
        let error = client
            .verify(BASE, &key(), &CancellationSource::default().token())
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
        assert_eq!(error.http_status, Some(status));
        assert_eq!(
            error.retry_after,
            retry.then_some(Duration::from_secs(if status == 429 { 60 } else { 30 }))
        );
        assert!(!format!("{error}").contains("private"));
    }
    for (values, delay) in [
        (vec!["1"], 1),
        (vec!["900"], 900),
        (vec!["0"], 60),
        (vec!["901"], 60),
        (vec!["-1"], 60),
        (vec!["date"], 60),
        (vec!["1", "2"], 60),
        (vec!["999999999999999999999999"], 60),
    ] {
        let client = client(move |_, _| {
            let mut response = response(429, vec![]);
            response.headers = values
                .iter()
                .map(|v| HttpHeader::public("Retry-After", *v))
                .collect();
            Ok(response)
        });
        assert_eq!(
            client
                .verify(BASE, &key(), &CancellationSource::default().token())
                .unwrap_err()
                .retry_after,
            Some(Duration::from_secs(delay))
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
        let error = client(move |_, _| Err(HttpError::new(code)))
            .verify(BASE, &key(), &CancellationSource::default().token())
            .unwrap_err();
        let retry = matches!(
            code,
            HttpErrorCode::Timeout
                | HttpErrorCode::NameResolutionFailed
                | HttpErrorCode::ConnectionFailed
                | HttpErrorCode::TlsFailed
                | HttpErrorCode::SendFailed
                | HttpErrorCode::ReceiveFailed
        );
        assert_eq!(error.http_error, Some(code));
        assert_eq!(
            error.disposition,
            if code == HttpErrorCode::Cancelled {
                DonBotDisposition::Cancelled
            } else if retry {
                DonBotDisposition::Retry
            } else {
                DonBotDisposition::Failed
            }
        );
    }
}

#[test]
fn cancellation_and_panics_cannot_publish_verified_identity() {
    let source = Arc::new(CancellationSource::default());
    let during = source.clone();
    let calls = Arc::new(Mutex::new(0));
    let count = calls.clone();
    let client = client(move |_, _| {
        *count.lock().unwrap() += 1;
        during.request_stop();
        Ok(response(200, serde_json::to_vec(&plain()).unwrap()))
    });
    for _ in 0..2 {
        assert_eq!(
            client
                .verify(BASE, &key(), &source.token())
                .unwrap_err()
                .disposition,
            DonBotDisposition::Cancelled
        );
    }
    assert_eq!(*calls.lock().unwrap(), 1);
    let throwing = DonBotClient::new(Arc::new(Http(
        |_: HttpRequest, _: &StopToken| -> Result<HttpResponse, HttpError> {
            panic!("synthetic private error")
        },
    )));
    let error = throwing
        .verify(BASE, &key(), &CancellationSource::default().token())
        .unwrap_err();
    assert_eq!(error.code, DonBotErrorCode::Internal);
    assert!(!format!("{error:?}").contains("private"));
}
