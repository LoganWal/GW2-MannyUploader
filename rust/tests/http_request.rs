use manny_uploader::application::*;
use std::time::Duration;

struct Body(u64);
impl HttpBodySource for Body {
    fn content_length(&self) -> u64 {
        self.0
    }
    fn read(&mut self, _: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
        panic!("validation must not pull body bytes")
    }
}
fn request() -> HttpRequest {
    HttpRequest::new(
        HttpMethod::Post,
        "https://api.example.test/upload?request=42",
    )
}
fn invalid(request: &HttpRequest) {
    assert_eq!(
        validate_http_request(request, HttpTransportPolicy::HttpsOnly)
            .unwrap_err()
            .code,
        HttpErrorCode::InvalidRequest
    );
}

#[test]
fn methods_defaults_and_production_urls_are_validated() {
    for (method, name) in [
        (HttpMethod::Get, "GET"),
        (HttpMethod::Post, "POST"),
        (HttpMethod::Put, "PUT"),
        (HttpMethod::Patch, "PATCH"),
        (HttpMethod::Delete, "DELETE"),
    ] {
        assert_eq!(method.name(), name);
        assert!(
            validate_http_request(
                &HttpRequest::new(method, "https://api.example.test"),
                HttpTransportPolicy::HttpsOnly
            )
            .is_ok()
        );
    }
    for url in [
        "https://api.example.test/a?b=c",
        "HTTPS://api.example.test:443",
        "https://[2001:db8::1]:444/a",
        "https://localhost/",
        "https://example.test./",
    ] {
        assert!(validate_http_url(url, HttpTransportPolicy::HttpsOnly).is_ok());
    }
    for url in [
        "",
        "example.test",
        "ftp://example.test/",
        "http://example.test/",
        "https://",
        "https://user:password@example.test/",
        "https://example.test/#fragment",
        "https://example.test/space here",
        "https://example.test/line\nbreak",
        "https://example.test/\u{96ea}",
        "https://example.test\\@evil.test/",
        "https://%65xample.test/",
        "https://example.test:0/",
        "https://example.test:65536/",
        "https://example.test:abc/",
        "https://example.test:/",
        "https://[invalid]/",
        "https://[::1]extra/",
        "https://::1/",
        "https://-example.test/",
        "https://example..test/",
    ] {
        assert_eq!(
            validate_http_url(url, HttpTransportPolicy::HttpsOnly)
                .unwrap_err()
                .code,
            HttpErrorCode::InvalidRequest
        );
    }
    let prefix = "https://example.test/";
    let boundary = format!("{prefix}{}", "a".repeat(MAX_HTTP_URL_BYTES - prefix.len()));
    assert!(validate_http_url(&boundary, HttpTransportPolicy::HttpsOnly).is_ok());
    assert!(validate_http_url(&(boundary + "a"), HttpTransportPolicy::HttpsOnly).is_err());
}

#[test]
fn plaintext_policy_permits_only_the_three_explicit_loopback_hosts() {
    for url in [
        "http://localhost/",
        "http://LOCALHOST:1234/",
        "http://127.0.0.1:65535/",
        "http://[::1]:80/",
    ] {
        assert!(validate_http_url(url, HttpTransportPolicy::HttpsOnly).is_err());
        assert!(validate_http_url(url, HttpTransportPolicy::PlaintextLoopbackForTests).is_ok());
    }
    for url in [
        "http://localhost.evil.test/",
        "http://127.1/",
        "http://2130706433/",
        "http://127.0.0.2/",
        "http://[::ffff:127.0.0.1]/",
        "http://localhost@evil.test/",
        "http://localhost:0/",
        "http://example.test/",
        "http://localhost.:80/",
    ] {
        assert!(validate_http_url(url, HttpTransportPolicy::PlaintextLoopbackForTests).is_err());
    }
}

#[test]
fn headers_enforce_framing_character_sets_and_sensitive_annotations() {
    for name in [
        "Host",
        "CONTENT-LENGTH",
        "transfer-encoding",
        "Connection",
        "Expect",
    ] {
        let mut r = request();
        r.headers.push(HttpHeader::public(name, "value"));
        invalid(&r);
    }
    for name in ["", "bad name", "bad:name", "bad\r\nname", "\u{96ea}"] {
        let mut r = request();
        r.headers.push(HttpHeader::public(name, "value"));
        invalid(&r);
    }
    for value in ["value\r\nInjected: true", "nul\0", "\u{7f}", "\u{96ea}"] {
        let mut r = request();
        r.headers.push(HttpHeader::public("X-Test", value));
        invalid(&r);
    }
    for name in [
        "Authorization",
        "PROXY-AUTHORIZATION",
        "Cookie",
        "Set-Cookie",
        "X-Api-Key",
        "x-gw2-api-key",
    ] {
        assert!(is_sensitive_http_header_name(name));
        let mut r = request();
        r.headers.push(HttpHeader::public(name, "synthetic-marker"));
        invalid(&r);
        assert_eq!(r.headers[0].redacted_value(), "[REDACTED]");
        assert!(!format!("{:?}", r.headers[0]).contains("synthetic-marker"));
        r.headers[0] =
            HttpHeader::sensitive(name, SecretValue::from_bytes(b"synthetic-marker".to_vec()));
        assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
    }
    let mut r = request();
    r.headers.push(HttpHeader::public(
        "!#$%&'*+-.^_`|~",
        " \tprintable ASCII ~",
    ));
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
}

#[test]
fn header_count_individual_and_total_byte_bounds_are_exact() {
    let mut r = request();
    r.headers = (0..MAX_HTTP_HEADER_COUNT)
        .map(|_| HttpHeader::public("X", ""))
        .collect();
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
    r.headers.push(HttpHeader::public("X", ""));
    invalid(&r);
    r.headers = vec![HttpHeader::public(
        "X".repeat(MAX_HTTP_HEADER_NAME_BYTES),
        "a".repeat(MAX_HTTP_HEADER_VALUE_BYTES),
    )];
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
    r.headers[0] = HttpHeader::public("X".repeat(MAX_HTTP_HEADER_NAME_BYTES + 1), "a");
    invalid(&r);
    r.headers[0] = HttpHeader::public("X", "a".repeat(MAX_HTTP_HEADER_VALUE_BYTES + 1));
    invalid(&r);
    // Four five-byte framing/name overheads plus these values total exactly 32 KiB.
    r.headers = (0..4)
        .map(|_| HttpHeader::public("X", "a".repeat(8192 - 5)))
        .collect();
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
    r.headers[0] = HttpHeader::public("X", "a".repeat(8192 - 4));
    invalid(&r);
}

#[test]
fn bodies_are_bounded_without_reads_and_get_cannot_have_a_body() {
    let mut r = request();
    r.body = Some(Box::new(Body(MAX_HTTP_REQUEST_BODY_BYTES)));
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
    r.method = HttpMethod::Get;
    invalid(&r);
    r.method = HttpMethod::Patch;
    for length in [0, MAX_HTTP_REQUEST_BODY_BYTES + 1, u64::MAX] {
        r.body = Some(Box::new(Body(length)));
        invalid(&r);
    }
    struct Panics;
    impl HttpBodySource for Panics {
        fn content_length(&self) -> u64 {
            panic!("synthetic metadata panic")
        }
        fn read(&mut self, _: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
            unreachable!()
        }
    }
    r.body = Some(Box::new(Panics));
    assert_eq!(
        validate_http_request(&r, HttpTransportPolicy::HttpsOnly)
            .unwrap_err()
            .code,
        HttpErrorCode::Internal
    );
}

#[test]
fn timeout_and_response_limit_boundaries_are_enforced() {
    let mut r = request();
    assert_eq!(r.timeouts.connect, Duration::from_secs(10));
    assert_eq!(r.timeouts.operation, Duration::from_secs(120));
    assert_eq!(r.timeouts.stalled_transfer, Duration::from_secs(30));
    assert_eq!(r.response_limits.max_body_bytes, 1024 * 1024);
    for (connect, operation, stall) in [
        (0, 120000, 30000),
        (60001, 120000, 30000),
        (10000, 0, 30000),
        (10000, 900001, 30000),
        (10000, 120000, 999),
        (10000, 120000, 900001),
    ] {
        r.timeouts = HttpTimeouts {
            connect: Duration::from_millis(connect),
            operation: Duration::from_millis(operation),
            stalled_transfer: Duration::from_millis(stall),
        };
        invalid(&r);
    }
    r.timeouts = HttpTimeouts {
        connect: Duration::from_secs(60),
        operation: Duration::from_secs(900),
        stalled_transfer: Duration::from_secs(900),
    };
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
    for (headers, body) in [
        (0, 1),
        (1, 0),
        (MAX_HTTP_RESPONSE_HEADER_BYTES + 1, 1),
        (1, MAX_HTTP_RESPONSE_BODY_BYTES + 1),
    ] {
        r.response_limits = HttpResponseLimits {
            max_header_bytes: headers,
            max_body_bytes: body,
        };
        invalid(&r);
    }
    r.response_limits = HttpResponseLimits {
        max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
        max_body_bytes: MAX_HTTP_RESPONSE_BODY_BYTES,
    };
    assert!(validate_http_request(&r, HttpTransportPolicy::HttpsOnly).is_ok());
}

#[test]
fn request_response_and_error_formatting_never_reveals_payloads_or_urls() {
    let mut r = HttpRequest::new(
        HttpMethod::Post,
        "https://api.example.test/token?secret=synthetic-marker",
    );
    r.headers.push(HttpHeader::sensitive(
        "X-Custom",
        SecretValue::from_bytes(b"synthetic-marker".to_vec()),
    ));
    let response = HttpResponse {
        status_code: 401,
        headers: vec![HttpHeader::sensitive(
            "Set-Cookie",
            SecretValue::from_bytes(b"synthetic-marker".to_vec()),
        )],
        body: SecretValue::from_bytes(b"synthetic-marker".to_vec()),
    };
    let error = HttpError {
        code: HttpErrorCode::BodyReadFailed,
        transport_code: Some(42),
        body_error: Some(HttpBodyErrorCode::ReadFailed),
        system_error: Some(5),
    };
    let diagnostics = format!("{r:?} {response:?} {error:?} {error} {:?}", r.headers);
    assert!(!diagnostics.contains("synthetic-marker"));
    assert!(!diagnostics.contains("example.test"));
    assert_eq!(
        HttpHeader::public("X-Request-Id", "public-value").redacted_value(),
        "public-value"
    );
}
