use manny_uploader::application::*;
use manny_uploader::http::ResponseCollector;

fn collector(headers: usize, body: usize) -> ResponseCollector {
    ResponseCollector::new(HttpResponseLimits {
        max_header_bytes: headers,
        max_body_bytes: body,
    })
    .unwrap()
}
fn token() -> StopToken {
    CancellationSource::default().token()
}
fn start(collector: &mut ResponseCollector, status: u16) {
    collector
        .header(format!("HTTP/1.1 {status} Test\r\n").as_bytes(), &token())
        .unwrap();
    collector.header(b"\r\n", &token()).unwrap();
}

#[test]
fn completed_error_statuses_are_responses_with_owned_binary_bodies() {
    for status in [200, 204, 301, 400, 401, 429, 500, 599] {
        let mut response = collector(1024, 4);
        start(&mut response, status);
        response.body(b"\0\xff", &token()).unwrap();
        response.body(b"AB", &token()).unwrap();
        let result = response.finish(status, &token()).unwrap();
        assert_eq!(result.status_code, status);
        assert_eq!(result.body.expose(), b"\0\xffAB");
        assert!(!format!("{result:?}").contains("AB"));
    }
}

#[test]
fn interim_headers_are_replaced_and_duplicates_and_trailers_are_preserved() {
    let mut response = collector(1024, 1);
    for line in [
        b"HTTP/1.1 100 Continue\r\n".as_slice(),
        b"X-Interim: gone\r\n",
        b"\r\n",
        b"HTTP/1.1 103 Early Hints\r\n",
        b"\r\n",
        b"HTTP/1.1 200 OK\r\n",
        b"X-Repeat: one\r\n",
        b"X-Repeat:\t two \t\r\n",
        b"Set-Cookie: synthetic-secret\r\n",
        b"X-Binary: \xff\r\n",
        b"\r\n",
    ] {
        response.header(line, &token()).unwrap();
    }
    response.body(b"a", &token()).unwrap();
    response.header(b"X-Trailer: three\r\n", &token()).unwrap();
    let result = response.finish(200, &token()).unwrap();
    assert_eq!(result.headers.len(), 5);
    assert_eq!(result.headers[0].name(), "X-Repeat");
    assert_eq!(result.headers[0].expose_value(), b"one");
    assert_eq!(result.headers[1].expose_value(), b"two");
    assert_eq!(
        result.headers[2].sensitivity(),
        HttpHeaderSensitivity::Sensitive
    );
    assert!(!format!("{:?}", result.headers[2]).contains("synthetic-secret"));
    assert_eq!(result.headers[3].expose_value(), b"\xff");
    assert_eq!(result.headers[4].name(), "X-Trailer");
}

#[test]
fn header_budget_counts_status_lines_interim_blocks_and_terminators() {
    let lines: [&[u8]; 6] = [
        b"HTTP/1.1 100 Continue\r\n",
        b"X: a\r\n",
        b"\r\n",
        b"HTTP/1.1 200 OK\r\n",
        b"X: b\r\n",
        b"\r\n",
    ];
    let length = lines.iter().map(|line| line.len()).sum();
    let mut exact = collector(length, 1);
    for line in lines {
        exact.header(line, &token()).unwrap();
    }
    assert!(exact.finish(200, &token()).is_ok());
    let mut short = collector(length - 1, 1);
    for line in &lines[..5] {
        short.header(line, &token()).unwrap();
    }
    let error = short.header(lines[5], &token()).unwrap_err();
    assert_eq!(error.code, HttpErrorCode::ResponseTooLarge);
    assert_eq!(short.body(b"", &token()), Err(error));
    assert_eq!(short.finish(200, &token()).unwrap_err(), error);
}

#[test]
fn body_overflow_discards_partial_response_and_remains_terminal() {
    let mut response = collector(1024, 3);
    start(&mut response, 200);
    response.body(b"abc", &token()).unwrap();
    let error = response.body(b"d", &token()).unwrap_err();
    assert_eq!(error.code, HttpErrorCode::ResponseTooLarge);
    assert_eq!(response.header(b"X: value\r\n", &token()), Err(error));
    assert_eq!(response.finish(200, &token()).unwrap_err(), error);
}

#[test]
fn malformed_status_headers_and_unfinished_responses_fail_closed() {
    for line in [
        b"HTTP/1.1 20 OK\r\n".as_slice(),
        b"HTTP/1.1 2000 OK\r\n",
        b"HTTP/1.1 600 Bad\r\n",
        b"HTTP/1.1 101 Upgrade\r\n",
        b"HTTP/9 200 OK\r\n",
        b"HTTP/1.1 200 OK",
        b"HTTP/1.1 200\0secret\r\n",
        b"X: no status\r\n",
    ] {
        let mut response = collector(1024, 1);
        assert_eq!(
            response.header(line, &token()).unwrap_err().code,
            HttpErrorCode::ProtocolError
        );
        assert_eq!(
            response.finish(200, &token()).unwrap_err().code,
            HttpErrorCode::ProtocolError
        );
    }
    for line in [
        b"missing colon\r\n".as_slice(),
        b": no name\r\n",
        b" X: space\r\n",
        b"X: bad\rinside\r\n",
        b"X: bad\ninside\r\n",
        b"X: bad\x7f\r\n",
        b"X: bad\x01\r\n",
    ] {
        let mut response = collector(1024, 1);
        start(&mut response, 200);
        assert_eq!(
            response.header(line, &token()).unwrap_err().code,
            HttpErrorCode::ProtocolError
        );
    }
    for status in [None, Some(100), Some(200)] {
        let mut response = collector(1024, 1);
        if let Some(status) = status {
            response
                .header(format!("HTTP/1.1 {status} Test\r\n").as_bytes(), &token())
                .unwrap();
        }
        assert_eq!(
            response.finish(200, &token()).unwrap_err().code,
            HttpErrorCode::ProtocolError
        );
    }
    let mut response = collector(1024, 1);
    start(&mut response, 200);
    assert_eq!(
        response.finish(201, &token()).unwrap_err().code,
        HttpErrorCode::ProtocolError
    );
}

#[test]
fn bodies_require_final_headers_and_final_status_cannot_be_replaced() {
    for interim in [false, true] {
        let mut response = collector(1024, 1);
        if interim {
            start(&mut response, 100);
        }
        assert_eq!(
            response.body(b"x", &token()).unwrap_err().code,
            HttpErrorCode::ProtocolError
        );
    }
    let mut response = collector(1024, 1);
    start(&mut response, 200);
    assert_eq!(
        response
            .header(b"HTTP/1.1 301 Redirect\r\n", &token())
            .unwrap_err()
            .code,
        HttpErrorCode::ProtocolError
    );
}

#[test]
fn cancellation_is_checked_at_headers_body_and_completion() {
    for stage in 0..3 {
        let cancel = CancellationSource::default();
        let mut response = collector(1024, 1);
        start(&mut response, 200);
        cancel.request_stop();
        let error = match stage {
            0 => response.header(b"X: a\r\n", &cancel.token()).unwrap_err(),
            1 => response.body(b"x", &cancel.token()).unwrap_err(),
            _ => response.finish(200, &cancel.token()).unwrap_err(),
        };
        assert_eq!(error.code, HttpErrorCode::Cancelled);
    }
}

#[test]
fn collector_bounds_are_validated_before_accepting_bytes() {
    for (headers, body) in [
        (0, 1),
        (1, 0),
        (MAX_HTTP_RESPONSE_HEADER_BYTES + 1, 1),
        (1, MAX_HTTP_RESPONSE_BODY_BYTES + 1),
    ] {
        assert!(matches!(
            ResponseCollector::new(HttpResponseLimits {
                max_header_bytes: headers,
                max_body_bytes: body
            }),
            Err(HttpError {
                code: HttpErrorCode::InvalidRequest,
                ..
            })
        ));
    }
    assert!(
        ResponseCollector::new(HttpResponseLimits {
            max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
            max_body_bytes: MAX_HTTP_RESPONSE_BODY_BYTES
        })
        .is_ok()
    );
}
