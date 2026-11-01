use super::*;
use manny_core::http::{MemoryBodySource, MultipartFormData, MultipartPart};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

struct Server {
    url: String,
    worker: Option<JoinHandle<Vec<u8>>>,
}
impl Server {
    fn new(handler: impl FnOnce(TcpStream) -> Vec<u8> + Send + 'static) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let url = format!("http://{}/fixture", listener.local_addr().unwrap());
        let worker = thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(5);
            loop {
                match listener.accept() {
                    Ok((stream, _)) => {
                        stream.set_nonblocking(false).unwrap();
                        stream
                            .set_read_timeout(Some(Duration::from_secs(3)))
                            .unwrap();
                        stream
                            .set_write_timeout(Some(Duration::from_secs(3)))
                            .unwrap();
                        return handler(stream);
                    }
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                        assert!(
                            Instant::now() < deadline,
                            "test server timed out waiting for connection"
                        );
                        thread::sleep(Duration::from_millis(5));
                    }
                    Err(_) => panic!("test server accept failed"),
                }
            }
        });
        Self {
            url,
            worker: Some(worker),
        }
    }
    fn reply(response: &[u8]) -> Self {
        let response = response.to_vec();
        Self::new(move |mut stream| {
            let request = read_request(&mut stream);
            let _ = stream.write_all(&response);
            request
        })
    }
    fn finish(mut self) -> Vec<u8> {
        self.worker.take().unwrap().join().unwrap()
    }
}
impl Drop for Server {
    fn drop(&mut self) {
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
fn read_headers(stream: &mut TcpStream) -> Vec<u8> {
    let mut request = Vec::new();
    while !request.ends_with(b"\r\n\r\n") {
        assert!(request.len() < 40 * 1024);
        let mut byte = [0];
        stream.read_exact(&mut byte).unwrap();
        request.push(byte[0]);
    }
    request
}
fn read_request(stream: &mut TcpStream) -> Vec<u8> {
    let mut request = read_headers(stream);
    let headers = std::str::from_utf8(&request).unwrap();
    let length = headers
        .lines()
        .filter_map(|line| line.split_once(':'))
        .find(|(name, _)| name.eq_ignore_ascii_case("Content-Length"))
        .map(|(_, value)| value.trim().parse::<usize>().unwrap())
        .unwrap_or(0);
    assert!(length <= 1024 * 1024);
    let offset = request.len();
    request.resize(offset + length, 0);
    stream.read_exact(&mut request[offset..]).unwrap();
    request
}
fn client() -> CurlHttpClient {
    CurlHttpClient::new(HttpTransportPolicy::PlaintextLoopbackForTests).unwrap()
}
fn token() -> StopToken {
    CancellationSource::default().token()
}

#[test]
fn native_backend_is_pinned_schannel_and_runtime_users_can_overlap() {
    let first = client();
    let second = client();
    let version = first.backend_version().unwrap();
    assert!(
        version.starts_with("libcurl/8.21.0 "),
        "unexpected pinned backend"
    );
    assert!(version.contains("Schannel"));
    {
        let _lock = RUNTIME_USERS.lock().unwrap_or_else(|e| e.into_inner());
        // SAFETY: A live client owns global initialization. Age zero exposes the original ABI prefix.
        let info = unsafe { &*ffi::curl_version_info(0) };
        assert_eq!(info.version_num, 0x081500);
        let mut protocols = Vec::new();
        for index in 0..16 {
            // SAFETY: libcurl owns a null-terminated protocol-pointer array for the library lifetime.
            let pointer = unsafe { *info.protocols.add(index) };
            if pointer.is_null() {
                break;
            }
            // SAFETY: Each non-null protocol entry is a library-owned NUL-terminated string.
            protocols.push(unsafe { CStr::from_ptr(pointer) }.to_bytes().to_vec());
        }
        assert_eq!(protocols, [b"http".to_vec(), b"https".to_vec()]);
        assert_ne!(info.features & (1 << 2), 0);
        assert_eq!(
            info.features & ((1 << 3) | (1 << 16) | (1 << 23) | (1 << 24) | (1 << 26) | (1 << 28)),
            0
        );
    }
    drop(first);
    assert_eq!(second.backend_version().unwrap(), version);
    thread::scope(|scope| {
        for _ in 0..4 {
            scope.spawn(|| {
                for _ in 0..20 {
                    let _ = client();
                }
            });
        }
    });
    let server = Server::reply(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    assert_eq!(
        second
            .execute(HttpRequest::new(HttpMethod::Get, &server.url), &token())
            .unwrap()
            .status_code,
        200
    );
    server.finish();
}

#[test]
fn native_requests_stream_all_methods_and_exact_owned_bodies() {
    for method in [
        HttpMethod::Get,
        HttpMethod::Post,
        HttpMethod::Put,
        HttpMethod::Patch,
        HttpMethod::Delete,
    ] {
        let server = Server::reply(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nX-Repeat: one\r\nX-Repeat: two\r\nSet-Cookie: synthetic-secret\r\n\r\ndata");
        let mut request = HttpRequest::new(method, &server.url);
        request.headers.push(HttpHeader::public("X-Empty", ""));
        request.headers.push(HttpHeader::sensitive(
            "Authorization",
            SecretValue::from_bytes(b"Bearer synthetic-fixture".to_vec()),
        ));
        if method != HttpMethod::Get {
            request.body = Some(Box::new(
                MemoryBodySource::new(b"binary\0\xff".to_vec()).unwrap(),
            ));
        }
        let response = client().execute(request, &token()).unwrap();
        assert_eq!(response.body.expose(), b"data");
        assert_eq!(
            response
                .headers
                .iter()
                .filter(|h| h.name() == "X-Repeat")
                .count(),
            2
        );
        assert!(
            response
                .headers
                .iter()
                .find(|h| h.name() == "Set-Cookie")
                .unwrap()
                .sensitivity()
                == HttpHeaderSensitivity::Sensitive
        );
        let wire = server.finish();
        assert!(
            wire.windows(b"X-Empty:\r\n".len())
                .any(|w| w == b"X-Empty:\r\n")
        );
        assert!(wire.starts_with(format!("{} /fixture HTTP/1.1\r\n", method.name()).as_bytes()));
        if method != HttpMethod::Get {
            assert!(wire.ends_with(b"\r\n\r\nbinary\0\xff"));
        }
        assert!(
            !wire
                .windows(b"Transfer-Encoding:".len())
                .any(|w| w == b"Transfer-Encoding:")
        );
        assert!(!wire.windows(b"Expect:".len()).any(|w| w == b"Expect:"));
    }
}

#[test]
fn native_multipart_preserves_streamed_framing_and_content_type() {
    let server = Server::reply(b"HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    let form = MultipartFormData::with_boundary(
        vec![MultipartPart {
            name: "file".into(),
            filename: Some("upload.zevtc".into()),
            content_type: Some("application/octet-stream".into()),
            body: Box::new(MemoryBodySource::new(b"fixture".to_vec()).unwrap()),
        }],
        "boundary",
    )
    .unwrap();
    let mut request = HttpRequest::new(HttpMethod::Post, &server.url);
    request
        .headers
        .push(HttpHeader::public("Content-Type", form.content_type));
    request.body = Some(form.body);
    assert_eq!(
        client().execute(request, &token()).unwrap().status_code,
        201
    );
    let wire = server.finish();
    assert!(
        String::from_utf8_lossy(&wire)
            .contains("Content-Type: multipart/form-data; boundary=boundary\r\n")
    );
    assert!(wire.ends_with(b"\r\n\r\nfixture\r\n--boundary--\r\n"));
}

#[test]
fn native_redirects_are_returned_without_following_and_error_statuses_succeed() {
    for status in [302, 400, 429, 500] {
        // Following this unsupported URL would fail, so a completed 302 proves it was not followed.
        let response = format!(
            "HTTP/1.1 {status} Test\r\nLocation: file:///never-follow\r\nContent-Length: 0\r\n\r\n"
        );
        let server = Server::reply(response.as_bytes());
        let result = client()
            .execute(HttpRequest::new(HttpMethod::Get, &server.url), &token())
            .unwrap();
        assert_eq!(result.status_code, status);
        server.finish();
    }
}

#[test]
fn native_response_limits_cover_declared_and_chunked_bodies_and_headers() {
    for (response, header_limit, body_limit) in [
        (
            b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndata".as_slice(),
            1024,
            3,
        ),
        (
            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nda\r\n2\r\nta\r\n0\r\n\r\n",
            1024,
            3,
        ),
        (
            b"HTTP/1.1 200 OK\r\nX-Large: value\r\nContent-Length: 0\r\n\r\n",
            20,
            10,
        ),
    ] {
        let server = Server::reply(response);
        let mut request = HttpRequest::new(HttpMethod::Get, &server.url);
        request.response_limits = HttpResponseLimits {
            max_header_bytes: header_limit,
            max_body_bytes: body_limit,
        };
        assert_eq!(
            client().execute(request, &token()).unwrap_err().code,
            HttpErrorCode::ResponseTooLarge
        );
        server.finish();
    }
}

#[test]
fn native_timeouts_and_cancellation_are_distinct() {
    for cancelled in [false, true] {
        let cancel = std::sync::Arc::new(CancellationSource::default());
        let server_cancel = cancel.clone();
        let server = Server::new(move |mut stream| {
            let request = read_request(&mut stream);
            if cancelled {
                server_cancel.request_stop();
            }
            let mut byte = [0];
            let _ = stream.read(&mut byte);
            request
        });
        let mut request = HttpRequest::new(HttpMethod::Get, &server.url);
        request.timeouts.operation = Duration::from_millis(if cancelled { 2500 } else { 100 });
        let start = Instant::now();
        let error = client().execute(request, &cancel.token()).unwrap_err();
        assert_eq!(
            error.code,
            if cancelled {
                HttpErrorCode::Cancelled
            } else {
                HttpErrorCode::Timeout
            }
        );
        assert!(start.elapsed() < Duration::from_secs(3));
        server.finish();
    }
}

struct BadSource(u8);
impl HttpBodySource for BadSource {
    fn content_length(&self) -> u64 {
        1
    }
    fn read(&mut self, bytes: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
        match self.0 {
            0 => Ok(0),
            1 => {
                bytes.fill(1);
                Ok(bytes.len())
            }
            _ => panic!("synthetic upload panic"),
        }
    }
}
#[test]
fn native_source_failures_never_escape_the_c_callback() {
    for mode in 0..3 {
        let server = Server::new(|mut stream| {
            let mut bytes = Vec::new();
            let _ = stream.read_to_end(&mut bytes);
            bytes
        });
        let mut request = HttpRequest::new(HttpMethod::Post, &server.url);
        request.body = Some(Box::new(BadSource(mode)));
        let error = client().execute(request, &token()).unwrap_err();
        assert_eq!(error.code, HttpErrorCode::BodyReadFailed);
        assert!(!format!("{error}").contains("synthetic"));
        server.finish();
    }
}

#[test]
fn native_validation_and_precancellation_fail_before_connection() {
    let production = CurlHttpClient::new(HttpTransportPolicy::HttpsOnly).unwrap();
    assert_eq!(
        production
            .execute(
                HttpRequest::new(HttpMethod::Get, "http://127.0.0.1:1"),
                &token()
            )
            .unwrap_err()
            .code,
        HttpErrorCode::InvalidRequest
    );
    let cancel = CancellationSource::default();
    cancel.request_stop();
    assert_eq!(
        production
            .execute(
                HttpRequest::new(HttpMethod::Get, "https://example.invalid"),
                &cancel.token()
            )
            .unwrap_err()
            .code,
        HttpErrorCode::Cancelled
    );
    assert_eq!(
        client()
            .execute(
                HttpRequest::new(HttpMethod::Get, "http://127.0.0.2:1"),
                &token()
            )
            .unwrap_err()
            .code,
        HttpErrorCode::InvalidRequest
    );
}

#[test]
fn native_interim_headers_and_chunked_trailers_use_the_final_response() {
    let server = Server::reply(b"HTTP/1.1 103 Early Hints\r\nX-Interim: gone\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Final: kept\r\n\r\n2\r\nhi\r\n0\r\nX-Trailer: kept\r\n\r\n");
    let response = client()
        .execute(HttpRequest::new(HttpMethod::Get, &server.url), &token())
        .unwrap();
    assert_eq!(response.body.expose(), b"hi");
    assert!(
        !response
            .headers
            .iter()
            .any(|header| header.name() == "X-Interim")
    );
    assert!(
        response
            .headers
            .iter()
            .any(|header| header.name() == "X-Trailer")
    );
    server.finish();
}

struct LargeSource(std::sync::Arc<std::sync::atomic::AtomicUsize>);
impl HttpBodySource for LargeSource {
    fn content_length(&self) -> u64 {
        MAX_HTTP_REQUEST_BODY_BYTES
    }
    fn read(&mut self, bytes: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
        self.0
            .fetch_max(bytes.len(), std::sync::atomic::Ordering::Relaxed);
        bytes.fill(b'x');
        Ok(bytes.len())
    }
}
#[test]
fn native_large_upload_cancels_without_allocating_its_declared_length() {
    let cancel = std::sync::Arc::new(CancellationSource::default());
    let server_cancel = cancel.clone();
    let server = Server::new(move |mut stream| {
        let headers = read_headers(&mut stream);
        let mut buffer = [0; 1024];
        stream.read_exact(&mut buffer).unwrap();
        server_cancel.request_stop();
        while let Ok(count) = stream.read(&mut buffer) {
            if count == 0 {
                break;
            }
        }
        headers
    });
    let maximum = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let mut request = HttpRequest::new(HttpMethod::Post, &server.url);
    request.body = Some(Box::new(LargeSource(maximum.clone())));
    let start = Instant::now();
    assert_eq!(
        client().execute(request, &cancel.token()).unwrap_err().code,
        HttpErrorCode::Cancelled
    );
    assert!(start.elapsed() < Duration::from_secs(3));
    let maximum = maximum.load(std::sync::atomic::Ordering::Relaxed);
    assert!(maximum > 0 && maximum <= 1024 * 1024);
    server.finish();
}

#[test]
fn native_https_never_falls_back_to_plaintext() {
    let server = Server::new(|mut stream| {
        let mut bytes = [0; 4096];
        let count = stream.read(&mut bytes).unwrap();
        let _ = stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        bytes[..count].to_vec()
    });
    let url = server.url.replacen("http:", "https:", 1);
    let production = CurlHttpClient::new(HttpTransportPolicy::HttpsOnly).unwrap();
    let mut request = HttpRequest::new(HttpMethod::Get, url);
    request.timeouts.operation = Duration::from_secs(2);
    assert_eq!(
        production.execute(request, &token()).unwrap_err().code,
        HttpErrorCode::TlsFailed
    );
    let wire = server.finish();
    assert!(!wire.starts_with(b"GET"));
}

#[test]
fn backend_error_mapping_retains_only_numeric_diagnostics() {
    for (code, category) in [
        (1, HttpErrorCode::InvalidRequest),
        (2, HttpErrorCode::InitializationFailed),
        (6, HttpErrorCode::NameResolutionFailed),
        (7, HttpErrorCode::ConnectionFailed),
        (28, HttpErrorCode::Timeout),
        (35, HttpErrorCode::TlsFailed),
        (60, HttpErrorCode::TlsFailed),
        (55, HttpErrorCode::SendFailed),
        (56, HttpErrorCode::ReceiveFailed),
        (63, HttpErrorCode::ResponseTooLarge),
        (8, HttpErrorCode::ProtocolError),
        (999, HttpErrorCode::Internal),
    ] {
        let error = map_backend_error(code);
        assert_eq!(error.code, category);
        assert_eq!(error.transport_code, Some(i64::from(code)));
    }
}

#[test]
#[ignore = "contacts a public HTTPS service, run explicitly for the Schannel release gate"]
fn live_schannel_https_probe() {
    let client = CurlHttpClient::new(HttpTransportPolicy::HttpsOnly).unwrap();
    let response = client
        .execute(
            HttpRequest::new(HttpMethod::Get, "https://example.com/"),
            &token(),
        )
        .unwrap();
    assert_eq!(response.status_code, 200);
    assert!(!response.body.is_empty());
}
