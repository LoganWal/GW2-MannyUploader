use manny_uploader::{application::*, domain::TwitchDeliveryStatus, providers::TwitchClient};
use serde_json::{Value, json};
use std::{
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};

const CLIENT: &str = "syntheticclient123";
const TOKEN: &[u8] = b"synthetic-token+/=&\"";
const OPERATIONS: [TwitchOperation; 6] = [
    TwitchOperation::Start,
    TwitchOperation::Poll,
    TwitchOperation::Validate,
    TwitchOperation::Refresh,
    TwitchOperation::Revoke,
    TwitchOperation::SendChat,
];
struct Http<F>(F);
impl<F: Fn(HttpRequest, &StopToken) -> Result<HttpResponse, HttpError> + Send + Sync> HttpClient
    for Http<F>
{
    fn execute(&self, request: HttpRequest, stop: &StopToken) -> Result<HttpResponse, HttpError> {
        (self.0)(request, stop)
    }
}
fn client(
    f: impl Fn(HttpRequest, &StopToken) -> Result<HttpResponse, HttpError> + Send + Sync + 'static,
) -> TwitchClient {
    TwitchClient::new(Arc::new(Http(f)), CLIENT.into()).unwrap()
}
fn secret() -> SecretValue {
    SecretValue::from_bytes(TOKEN.to_vec())
}
fn stop() -> StopToken {
    CancellationSource::default().token()
}
fn response(status_code: u16, bytes: Vec<u8>) -> HttpResponse {
    HttpResponse {
        status_code,
        headers: vec![],
        body: SecretValue::from_bytes(bytes),
    }
}
fn document(op: TwitchOperation) -> Value {
    match op {
        TwitchOperation::Start => {
            json!({"device_code":"synthetic-device","user_code":"ABC-DEF","verification_uri":"https://www.twitch.tv/activate?public=value","expires_in":1800,"interval":5})
        }
        TwitchOperation::Poll | TwitchOperation::Refresh => {
            json!({"access_token":"synthetic-access","refresh_token":"synthetic-refresh","expires_in":14400,"token_type":"bearer","scope":["user:write:chat"]})
        }
        TwitchOperation::Validate => {
            json!({"client_id":CLIENT,"user_id":"123","login":"synthetic_user","expires_in":14400,"scopes":["user:write:chat"]})
        }
        TwitchOperation::Revoke => Value::Null,
        TwitchOperation::SendChat => {
            json!({"data":[{"is_sent":true,"message_id":"synthetic-message-id"}]})
        }
    }
}
fn invoke(client: &TwitchClient, op: TwitchOperation) -> Result<(), TwitchError> {
    match op {
        TwitchOperation::Start => client.start_device_authorization(&stop()).map(|_| ()),
        TwitchOperation::Poll => client
            .poll_device_authorization(&secret(), &stop())
            .map(|_| ()),
        TwitchOperation::Validate => client.validate_access_token(&secret(), &stop()).map(|_| ()),
        TwitchOperation::Refresh => client.refresh_access_token(&secret(), &stop()).map(|_| ()),
        TwitchOperation::Revoke => client.revoke_access_token(&secret(), &stop()),
        TwitchOperation::SendChat => client
            .send_chat_message("123", "Synthetic \"chat\" \\ text", &secret(), &stop())
            .map(|_| ()),
    }
}
fn decode(op: TwitchOperation, bytes: Vec<u8>) -> Result<(), TwitchError> {
    invoke(&client(move |_, _| Ok(response(200, bytes.clone()))), op)
}
fn invalid(op: TwitchOperation, value: Value) {
    let error = decode(op, serde_json::to_vec(&value).unwrap()).unwrap_err();
    assert_eq!(error.code, TwitchErrorCode::InvalidResponse);
    assert_eq!(error.http_status, Some(200));
    assert_eq!(
        error.disposition,
        if op == TwitchOperation::Validate {
            TwitchDisposition::Reconnect
        } else {
            TwitchDisposition::Failed
        }
    );
    assert!(!format!("{error} {}", error.detail()).contains("synthetic"));
}
fn body(request: &mut HttpRequest, stop: &StopToken) -> Vec<u8> {
    let Some(source) = &mut request.body else {
        return vec![];
    };
    let mut result = Vec::new();
    let mut block = [0; 7];
    loop {
        let count = source.read(&mut block, stop).unwrap();
        if count == 0 {
            break;
        }
        result.extend_from_slice(&block[..count]);
    }
    assert_eq!(result.len() as u64, source.content_length());
    result
}

#[test]
fn every_primitive_has_exact_endpoint_headers_timeouts_and_body() {
    for op in OPERATIONS {
        let calls = Arc::new(AtomicUsize::new(0));
        let count = calls.clone();
        let c = client(move |mut request, stop| {
            count.fetch_add(1, Ordering::Relaxed);
            validate_http_request(&request, HttpTransportPolicy::HttpsOnly).unwrap();
            assert_eq!(
                request.timeouts,
                HttpTimeouts {
                    connect: Duration::from_secs(10),
                    operation: Duration::from_secs(30),
                    stalled_transfer: Duration::from_secs(30)
                }
            );
            assert_eq!(
                request.response_limits,
                HttpResponseLimits {
                    max_header_bytes: 65536,
                    max_body_bytes: 65536
                }
            );
            assert_eq!(
                request.method,
                if op == TwitchOperation::Validate {
                    HttpMethod::Get
                } else {
                    HttpMethod::Post
                }
            );
            assert!(
                request
                    .headers
                    .iter()
                    .any(|h| h.name() == "Accept" && h.expose_value() == b"application/json")
            );
            assert!(!format!("{request:?}").contains("synthetic-token"));
            assert!(!request.url.contains("synthetic"));
            let authorization = request.headers.iter().find(|h| h.name() == "Authorization");
            if matches!(op, TwitchOperation::Validate | TwitchOperation::SendChat) {
                let header = authorization.unwrap();
                assert_eq!(header.sensitivity(), HttpHeaderSensitivity::Sensitive);
                let mut expected = if op == TwitchOperation::Validate {
                    b"OAuth ".to_vec()
                } else {
                    b"Bearer ".to_vec()
                };
                expected.extend_from_slice(TOKEN);
                assert_eq!(header.expose_value(), expected);
            } else {
                assert!(authorization.is_none());
            }
            let bytes = body(&mut request, stop);
            match op {
                TwitchOperation::Start => {
                    assert_eq!(request.url, "https://id.twitch.tv/oauth2/device");
                    assert_eq!(
                        bytes,
                        b"client_id=syntheticclient123&scopes=user%3Awrite%3Achat"
                    );
                }
                TwitchOperation::Poll => {
                    assert_eq!(request.url, "https://id.twitch.tv/oauth2/token");
                    assert_eq!(bytes, b"client_id=syntheticclient123&scopes=user%3Awrite%3Achat&device_code=synthetic-token%2B%2F%3D%26%22&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code");
                }
                TwitchOperation::Validate => {
                    assert_eq!(request.url, "https://id.twitch.tv/oauth2/validate");
                    assert!(request.body.is_none());
                }
                TwitchOperation::Refresh => {
                    assert_eq!(request.url, "https://id.twitch.tv/oauth2/token");
                    assert_eq!(bytes, b"grant_type=refresh_token&refresh_token=synthetic-token%2B%2F%3D%26%22&client_id=syntheticclient123");
                }
                TwitchOperation::Revoke => {
                    assert_eq!(request.url, "https://id.twitch.tv/oauth2/revoke");
                    assert_eq!(
                        bytes,
                        b"client_id=syntheticclient123&token=synthetic-token%2B%2F%3D%26%22"
                    );
                }
                TwitchOperation::SendChat => {
                    assert_eq!(request.url, "https://api.twitch.tv/helix/chat/messages");
                    assert!(
                        request
                            .headers
                            .iter()
                            .any(|h| h.name() == "Client-Id"
                                && h.expose_value() == CLIENT.as_bytes())
                    );
                    let value: Value = serde_json::from_slice(&bytes).unwrap();
                    assert_eq!(
                        value,
                        json!({"broadcaster_id":"123","sender_id":"123","message":"Synthetic \"chat\" \\ text"})
                    );
                }
            }
            if op != TwitchOperation::Validate {
                assert!(request.headers.iter().any(|h| h.name() == "Content-Type"
                    && h.expose_value()
                        == if op == TwitchOperation::SendChat {
                            b"application/json".as_slice()
                        } else {
                            b"application/x-www-form-urlencoded".as_slice()
                        }));
            }
            Ok(response(200, serde_json::to_vec(&document(op)).unwrap()))
        });
        invoke(&c, op).unwrap();
        assert_eq!(calls.load(Ordering::Relaxed), 1);
    }
}

#[test]
fn device_success_and_pending_grants_preserve_owned_values() {
    let c = client(|_, _| {
        Ok(response(
            200,
            serde_json::to_vec(&document(TwitchOperation::Start)).unwrap(),
        ))
    });
    let authorization = c.start_device_authorization(&stop()).unwrap();
    assert_eq!(authorization.device_code.expose(), b"synthetic-device");
    assert_eq!(authorization.user_code, "ABC-DEF");
    assert_eq!(authorization.polling_interval, Duration::from_secs(5));
    assert_eq!(authorization.expires_in, Duration::from_secs(1800));
    assert!(!format!("{authorization:?}").contains("synthetic-device"));
    let c = client(|_, _| {
        Ok(response(
            400,
            br#"{"status":400,"message":"authorization_pending"}"#.to_vec(),
        ))
    });
    assert!(matches!(
        c.poll_device_authorization(&secret(), &stop()),
        Ok(TwitchDevicePollResult::Pending)
    ));
    let c = client(|_, _| {
        Ok(response(
            200,
            serde_json::to_vec(&document(TwitchOperation::Poll)).unwrap(),
        ))
    });
    let TwitchDevicePollResult::Granted(grant) =
        c.poll_device_authorization(&secret(), &stop()).unwrap()
    else {
        panic!("expected grant");
    };
    assert_eq!(grant.access_token.expose(), b"synthetic-access");
    assert_eq!(grant.refresh_token.expose(), b"synthetic-refresh");
    assert_eq!(grant.scopes, [TWITCH_CHAT_SCOPE]);
    assert!(!format!("{grant:?}").contains("synthetic-access"));
}

#[test]
fn pending_requires_exact_status_message_and_object_shape() {
    for value in [
        json!({"status":401,"message":"authorization_pending"}),
        json!({"status":400,"message":"slow_down"}),
        json!({"status":400}),
        json!({"message":"authorization_pending"}),
        json!([{"status":400,"message":"authorization_pending"}]),
        json!({"status":"400","message":"authorization_pending"}),
    ] {
        let bytes = serde_json::to_vec(&value).unwrap();
        let c = client(move |_, _| Ok(response(400, bytes.clone())));
        let error = c.poll_device_authorization(&secret(), &stop()).unwrap_err();
        assert_eq!(error.disposition, TwitchDisposition::Failed);
        assert_eq!(error.http_status, Some(400));
    }
}

#[test]
fn device_fields_have_strict_limits_and_activation_origin() {
    let op = TwitchOperation::Start;
    for (field, value) in [
        ("device_code", json!("")),
        ("device_code", json!("bad token")),
        ("device_code", json!("x".repeat(8193))),
        ("user_code", json!("")),
        ("user_code", json!("x".repeat(65))),
        ("user_code", json!("bad\ncode")),
        ("interval", json!(0)),
        ("interval", json!(301)),
        ("interval", json!(1.5)),
        ("expires_in", json!(-1)),
        ("expires_in", json!(0)),
        ("expires_in", json!(604801)),
        ("verification_uri", json!("http://www.twitch.tv/activate")),
        (
            "verification_uri",
            json!("https://www.twitch.tv/activate.evil"),
        ),
        (
            "verification_uri",
            json!("https://www.twitch.tv/activate#fragment"),
        ),
        (
            "verification_uri",
            json!("https://www.twitch.tv/activate?user@host"),
        ),
    ] {
        let mut value_base = document(op);
        value_base[field] = value;
        invalid(op, value_base);
    }
    for field in [
        "device_code",
        "user_code",
        "interval",
        "expires_in",
        "verification_uri",
    ] {
        let mut value = document(op);
        value.as_object_mut().unwrap().remove(field);
        invalid(op, value);
    }
    let mut valid = document(op);
    valid["interval"] = json!(300);
    valid["expires_in"] = json!(604800);
    valid["device_code"] = json!("x".repeat(8192));
    decode(op, serde_json::to_vec(&valid).unwrap()).unwrap();
}

#[test]
fn grants_accept_only_exact_scope_and_bearer_with_bounded_rotating_tokens() {
    for op in [TwitchOperation::Poll, TwitchOperation::Refresh] {
        for scope in [json!(TWITCH_CHAT_SCOPE), json!([TWITCH_CHAT_SCOPE])] {
            let mut value = document(op);
            value["scope"] = scope;
            value["token_type"] = json!("BeArEr");
            decode(op, serde_json::to_vec(&value).unwrap()).unwrap();
        }
        for (field, value) in [
            ("scope", json!([])),
            ("scope", json!([TWITCH_CHAT_SCOPE, TWITCH_CHAT_SCOPE])),
            ("scope", json!("user:write:chat chat:read")),
            ("scope", json!(["chat:read"])),
            ("token_type", json!("OAuth")),
            ("access_token", json!("")),
            ("refresh_token", json!("bad\ttoken")),
            ("access_token", json!("x".repeat(8193))),
            ("refresh_token", Value::Null),
            ("expires_in", json!(604801)),
        ] {
            let mut base = document(op);
            base[field] = value;
            invalid(op, base);
        }
        for field in [
            "access_token",
            "refresh_token",
            "scope",
            "expires_in",
            "token_type",
        ] {
            let mut value = document(op);
            value.as_object_mut().unwrap().remove(field);
            invalid(op, value);
        }
    }
}

#[test]
fn validation_binds_exact_client_identity_and_scope() {
    let op = TwitchOperation::Validate;
    let c = client(move |_, _| Ok(response(200, serde_json::to_vec(&document(op)).unwrap())));
    let identity = c.validate_access_token(&secret(), &stop()).unwrap();
    assert_eq!(identity.user_id, "123");
    assert_eq!(identity.login, "synthetic_user");
    assert_eq!(identity.scopes, [TWITCH_CHAT_SCOPE]);
    assert!(!format!("{identity:?}").contains("synthetic_user"));
    for (field, value) in [
        ("client_id", json!("otherclient")),
        ("user_id", json!("0")),
        ("user_id", json!("0123")),
        ("user_id", json!("+123")),
        ("user_id", json!("18446744073709551616")),
        ("login", json!("Upper")),
        ("login", json!("a".repeat(65))),
        ("scopes", json!([])),
        ("scopes", json!(TWITCH_CHAT_SCOPE)),
        ("scopes", json!([TWITCH_CHAT_SCOPE, "chat:read"])),
        ("expires_in", json!(0)),
    ] {
        let mut base = document(op);
        base[field] = value;
        invalid(op, base);
    }
}

#[test]
fn sent_and_dropped_chat_results_are_normalized_without_server_text() {
    let codes = [
        ("automod_held", TwitchDeliveryStatus::AutoMod),
        ("automod_caught", TwitchDeliveryStatus::AutoMod),
        ("blocked_term", TwitchDeliveryStatus::BlockedTerm),
        ("msg_duplicate", TwitchDeliveryStatus::Duplicate),
        ("rate_limited", TwitchDeliveryStatus::RateLimited),
        ("followers_only", TwitchDeliveryStatus::FollowersOnly),
        ("slow_mode", TwitchDeliveryStatus::SlowMode),
        ("subscribers_only", TwitchDeliveryStatus::SubscribersOnly),
        ("banned", TwitchDeliveryStatus::Restricted),
        ("channel_suspended", TwitchDeliveryStatus::Restricted),
        ("restricted", TwitchDeliveryStatus::Restricted),
        ("future_drop", TwitchDeliveryStatus::OtherDrop),
    ];
    for (code, status) in codes {
        let c = client(move |_, _| {
            Ok(response(200, serde_json::to_vec(&json!({"data":[{"is_sent":false,"drop_reason":{"code":code,"message":"SYNTHETIC-PRIVATE-SERVER-TEXT"}}]})).unwrap()))
        });
        let receipt = c
            .send_chat_message("123", "test", &secret(), &stop())
            .unwrap();
        assert_eq!(receipt.status, status);
        assert_eq!(receipt.message_id, None);
        assert!(!format!("{receipt:?}").contains("SYNTHETIC"));
    }
    let c = client(|_, _| {
        Ok(response(
            200,
            serde_json::to_vec(&document(TwitchOperation::SendChat)).unwrap(),
        ))
    });
    let receipt = c
        .send_chat_message("123", "test", &secret(), &stop())
        .unwrap();
    assert_eq!(receipt.status, TwitchDeliveryStatus::Sent);
    assert_eq!(receipt.message_id.as_deref(), Some("synthetic-message-id"));
}

#[test]
fn chat_responses_require_one_consistent_sent_or_dropped_item() {
    for value in [
        json!({"data":[]}),
        json!({"data":[{},{}]}),
        json!({"data":[{"is_sent":true}]}),
        json!({"data":[{"is_sent":true,"message_id":""}]}),
        json!({"data":[{"is_sent":true,"message_id":"x","drop_reason":{"code":"x","message":"x"}}]}),
        json!({"data":[{"is_sent":false,"message_id":"x","drop_reason":{"code":"x","message":"x"}}]}),
        json!({"data":[{"is_sent":false,"drop_reason":{"code":"x"}}]}),
        json!({"data":[{"is_sent":false,"drop_reason":{"code":"x","message":"bad\ntext"}}]}),
        json!({"data":[{"message_id":"x"}]}),
        json!({"data":[{"is_sent":true,"message_id":"x".repeat(257)}]}),
    ] {
        invalid(TwitchOperation::SendChat, value);
    }
}

#[test]
fn chat_input_checks_unicode_after_expansion_and_never_accepts_foreign_identity_inputs() {
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let c = client(move |_, _| {
        count.fetch_add(1, Ordering::Relaxed);
        Ok(response(
            200,
            serde_json::to_vec(&document(TwitchOperation::SendChat)).unwrap(),
        ))
    });
    c.send_chat_message(
        "18446744073709551615",
        &"\u{1f600}".repeat(500),
        &secret(),
        &stop(),
    )
    .unwrap();
    for message in [
        "".into(),
        "a".repeat(501),
        "\u{1f600}".repeat(501),
        "bad\ntext".into(),
        "bad\u{7f}".into(),
    ] {
        assert_eq!(
            c.send_chat_message("123", &message, &secret(), &stop())
                .unwrap_err()
                .code,
            TwitchErrorCode::InvalidInput
        );
    }
    for user in ["", "0", "0123", "-123", "18446744073709551616"] {
        assert!(
            c.send_chat_message(user, "test", &secret(), &stop())
                .is_err()
        );
    }
    assert_eq!(calls.load(Ordering::Relaxed), 1);
}

#[test]
fn every_http_status_class_is_operation_specific_and_never_retried_in_client() {
    for op in OPERATIONS {
        for status in [201, 204, 301, 400, 401, 403, 404, 408, 429, 500, 503] {
            let calls = Arc::new(AtomicUsize::new(0));
            let count = calls.clone();
            let c = client(move |_, _| {
                count.fetch_add(1, Ordering::Relaxed);
                Ok(response(status, b"SYNTHETIC-PRIVATE-ERROR".to_vec()))
            });
            let error = invoke(&c, op).unwrap_err();
            let reconnect = (op == TwitchOperation::Refresh && matches!(status, 400 | 401))
                || (matches!(op, TwitchOperation::Validate | TwitchOperation::SendChat)
                    && status == 401);
            let retry = matches!(status, 408 | 429 | 500 | 503);
            assert_eq!(
                error.disposition,
                if reconnect {
                    TwitchDisposition::Reconnect
                } else if retry {
                    TwitchDisposition::Retry
                } else {
                    TwitchDisposition::Failed
                }
            );
            assert_eq!(error.operation, op);
            assert_eq!(error.http_status, Some(status));
            assert_eq!(
                error.retry_after,
                retry.then_some(Duration::from_secs(if status == 429 { 60 } else { 30 }))
            );
            assert!(!format!("{error}").contains("PRIVATE"));
            assert_eq!(calls.load(Ordering::Relaxed), 1);
        }
    }
}

#[test]
fn retry_after_is_unique_numeric_positive_and_bounded() {
    for (values, expected) in [
        (vec!["15"], 15),
        (vec!["900"], 900),
        (vec!["0"], 60),
        (vec!["901"], 60),
        (vec![" 15"], 60),
        (vec!["9999999999999999999999999"], 60),
        (vec!["15", "15"], 60),
        (vec!["tomorrow"], 60),
    ] {
        let c = client(move |_, _| {
            let mut response = response(429, vec![]);
            response.headers = values
                .iter()
                .map(|v| HttpHeader::public("rEtRy-AfTeR", *v))
                .collect();
            Ok(response)
        });
        assert_eq!(
            c.revoke_access_token(&secret(), &stop())
                .unwrap_err()
                .retry_after,
            Some(Duration::from_secs(expected))
        );
    }
}

#[test]
fn transport_errors_cancel_and_contain_panics_without_secret_diagnostics() {
    for code in [
        HttpErrorCode::Cancelled,
        HttpErrorCode::Timeout,
        HttpErrorCode::NameResolutionFailed,
        HttpErrorCode::ConnectionFailed,
        HttpErrorCode::TlsFailed,
        HttpErrorCode::SendFailed,
        HttpErrorCode::ReceiveFailed,
        HttpErrorCode::InvalidRequest,
        HttpErrorCode::BodyReadFailed,
        HttpErrorCode::ResponseTooLarge,
        HttpErrorCode::ProtocolError,
        HttpErrorCode::UnsupportedEnvironment,
        HttpErrorCode::InitializationFailed,
        HttpErrorCode::Internal,
    ] {
        let c = client(move |_, _| Err(HttpError::new(code)));
        let error = c.revoke_access_token(&secret(), &stop()).unwrap_err();
        assert_eq!(error.http_error, Some(code));
        assert_eq!(
            error.disposition,
            match code {
                HttpErrorCode::Cancelled => TwitchDisposition::Cancelled,
                HttpErrorCode::Timeout
                | HttpErrorCode::NameResolutionFailed
                | HttpErrorCode::ConnectionFailed
                | HttpErrorCode::TlsFailed
                | HttpErrorCode::SendFailed
                | HttpErrorCode::ReceiveFailed => TwitchDisposition::Retry,
                _ => TwitchDisposition::Failed,
            }
        );
    }
    let c = client(|_, _| panic!("synthetic HTTP failure"));
    let error = c.revoke_access_token(&secret(), &stop()).unwrap_err();
    assert_eq!(error.code, TwitchErrorCode::Internal);
    assert!(!format!("{error}").contains("synthetic"));
    let cancelled = CancellationSource::default();
    cancelled.request_stop();
    assert_eq!(
        c.start_device_authorization(&cancelled.token())
            .unwrap_err()
            .disposition,
        TwitchDisposition::Cancelled
    );
    let cancellation = Arc::new(CancellationSource::default());
    let source = cancellation.clone();
    let c = client(move |_, _| {
        source.request_stop();
        Ok(response(200, vec![]))
    });
    assert_eq!(
        c.revoke_access_token(&secret(), &cancellation.token())
            .unwrap_err()
            .disposition,
        TwitchDisposition::Cancelled
    );
}

#[test]
fn invalid_credentials_and_client_configuration_never_reach_http() {
    let http = Arc::new(Http(
        |_: HttpRequest, _: &StopToken| -> Result<HttpResponse, HttpError> {
            panic!("unexpected network call");
        },
    ));
    for id in ["", "Upper", "has space", "with-dash"] {
        assert!(TwitchClient::new(http.clone(), id.into()).is_err());
    }
    assert!(TwitchClient::new(http.clone(), "x".repeat(129)).is_err());
    let c = TwitchClient::unconfigured(http);
    assert!(!c.configured());
    assert_eq!(
        c.start_device_authorization(&stop()).unwrap_err().code,
        TwitchErrorCode::Unconfigured
    );
    c.update_client_id(CLIENT.into()).unwrap();
    assert!(c.configured());
    assert!(c.update_client_id("invalid client".into()).is_err());
    for token in [
        SecretValue::default(),
        SecretValue::from_bytes(b"bad token".to_vec()),
        SecretValue::from_bytes(vec![b'a'; 8193]),
    ] {
        assert_eq!(
            c.validate_access_token(&token, &stop())
                .unwrap_err()
                .disposition,
            TwitchDisposition::Reconnect
        );
        assert_eq!(
            c.refresh_access_token(&token, &stop())
                .unwrap_err()
                .disposition,
            TwitchDisposition::Reconnect
        );
        assert_eq!(
            c.poll_device_authorization(&token, &stop())
                .unwrap_err()
                .code,
            TwitchErrorCode::InvalidInput
        );
    }
    c.update_client_id(String::new()).unwrap();
    assert!(!c.configured());
}

#[test]
fn responses_are_bounded_objects_with_strict_unicode_and_unique_known_fields() {
    for op in [
        TwitchOperation::Start,
        TwitchOperation::Poll,
        TwitchOperation::Validate,
        TwitchOperation::Refresh,
        TwitchOperation::SendChat,
    ] {
        for bytes in [
            b"[]".to_vec(),
            b"null".to_vec(),
            vec![b' '; 65537],
            b"{\"unknown\":\"\\ud800\"}".to_vec(),
            vec![0xff],
        ] {
            assert!(decode(op, bytes).is_err());
        }
        let mut value = document(op);
        value["future"] = json!({"nested": [1,true,"ok"]});
        decode(op, serde_json::to_vec(&value).unwrap()).unwrap();
        let bytes = serde_json::to_vec(&document(op)).unwrap();
        for length in 0..bytes.len() {
            assert!(decode(op, bytes[..length].to_vec()).is_err());
        }
    }
    assert!(decode(TwitchOperation::Poll, br#"{"access_token":"a","access_token":"b","refresh_token":"r","expires_in":1,"token_type":"bearer","scope":"user:write:chat"}"#.to_vec()).is_err());
}

#[test]
fn in_flight_validation_uses_captured_client_id_without_holding_configuration_lock() {
    let gate = Arc::new(std::sync::Barrier::new(2));
    let reached = gate.clone();
    let c = Arc::new(client(move |_, _| {
        reached.wait();
        reached.wait();
        Ok(response(
            200,
            serde_json::to_vec(&document(TwitchOperation::Validate)).unwrap(),
        ))
    }));
    let worker = c.clone();
    let result = std::thread::spawn(move || worker.validate_access_token(&secret(), &stop()));
    gate.wait();
    c.update_client_id("replacementclient".into()).unwrap();
    gate.wait();
    assert!(result.join().unwrap().is_ok());
}
