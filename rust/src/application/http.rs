use std::fmt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::time::Duration;

use super::{SecretValue, StopToken};

pub const MAX_HTTP_URL_BYTES: usize = 4096;
pub const MAX_HTTP_HEADER_COUNT: usize = 64;
pub const MAX_HTTP_HEADER_NAME_BYTES: usize = 128;
pub const MAX_HTTP_HEADER_VALUE_BYTES: usize = 8192;
pub const MAX_HTTP_REQUEST_HEADER_BYTES: usize = 32 * 1024;
pub const MAX_HTTP_REQUEST_BODY_BYTES: u64 = 512 * 1024 * 1024;
pub const MAX_HTTP_RESPONSE_HEADER_BYTES: usize = 64 * 1024;
pub const MAX_HTTP_RESPONSE_BODY_BYTES: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HttpMethod {
    Get,
    Post,
    Put,
    Patch,
    Delete,
}
impl HttpMethod {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Get => "GET",
            Self::Post => "POST",
            Self::Put => "PUT",
            Self::Patch => "PATCH",
            Self::Delete => "DELETE",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HttpHeaderSensitivity {
    Public,
    Sensitive,
}

pub struct HttpHeader {
    name: String,
    value: SecretValue,
    sensitivity: HttpHeaderSensitivity,
}
impl HttpHeader {
    pub fn public(name: impl Into<String>, value: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            value: SecretValue::from_bytes(value.into().into_bytes()),
            sensitivity: HttpHeaderSensitivity::Public,
        }
    }
    pub fn sensitive(name: impl Into<String>, value: SecretValue) -> Self {
        Self {
            name: name.into(),
            value,
            sensitivity: HttpHeaderSensitivity::Sensitive,
        }
    }
    pub fn name(&self) -> &str {
        &self.name
    }
    pub fn expose_value(&self) -> &[u8] {
        self.value.expose()
    }
    pub const fn sensitivity(&self) -> HttpHeaderSensitivity {
        self.sensitivity
    }
    pub fn redacted_value(&self) -> &str {
        if self.sensitivity == HttpHeaderSensitivity::Sensitive
            || is_sensitive_http_header_name(&self.name)
        {
            "[REDACTED]"
        } else {
            std::str::from_utf8(self.value.expose()).unwrap_or("[INVALID]")
        }
    }
}
impl fmt::Debug for HttpHeader {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("HttpHeader")
            .field("name", &self.name)
            .field("value", &self.redacted_value())
            .field("sensitivity", &self.sensitivity)
            .finish()
    }
}

pub fn is_sensitive_http_header_name(name: &str) -> bool {
    [
        "Authorization",
        "Proxy-Authorization",
        "Cookie",
        "Set-Cookie",
        "X-Api-Key",
        "X-Gw2-Api-Key",
    ]
    .iter()
    .any(|known| name.eq_ignore_ascii_case(known))
}
pub fn is_http_token_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&byte)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HttpBodyErrorCode {
    Cancelled,
    SourceUnavailable,
    SourceChanged,
    ReadFailed,
    Panicked,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HttpBodyError {
    pub code: HttpBodyErrorCode,
    pub system_error: Option<u32>,
}
impl HttpBodyError {
    pub const fn new(code: HttpBodyErrorCode) -> Self {
        Self {
            code,
            system_error: None,
        }
    }
}

/// Single-use source. Reads must make progress until content_length bytes, then report EOF.
pub trait HttpBodySource: Send {
    fn content_length(&self) -> u64;
    fn read(&mut self, destination: &mut [u8], stop: &StopToken) -> Result<usize, HttpBodyError>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HttpTimeouts {
    pub connect: Duration,
    pub operation: Duration,
    pub stalled_transfer: Duration,
}
impl Default for HttpTimeouts {
    fn default() -> Self {
        Self {
            connect: Duration::from_secs(10),
            operation: Duration::from_secs(120),
            stalled_transfer: Duration::from_secs(30),
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HttpResponseLimits {
    pub max_header_bytes: usize,
    pub max_body_bytes: usize,
}
impl Default for HttpResponseLimits {
    fn default() -> Self {
        Self {
            max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
            max_body_bytes: 1024 * 1024,
        }
    }
}

pub struct HttpRequest {
    pub method: HttpMethod,
    pub url: String,
    pub headers: Vec<HttpHeader>,
    pub body: Option<Box<dyn HttpBodySource>>,
    pub timeouts: HttpTimeouts,
    pub response_limits: HttpResponseLimits,
}
impl HttpRequest {
    pub fn new(method: HttpMethod, url: impl Into<String>) -> Self {
        Self {
            method,
            url: url.into(),
            headers: Vec::new(),
            body: None,
            timeouts: HttpTimeouts::default(),
            response_limits: HttpResponseLimits::default(),
        }
    }
}
impl fmt::Debug for HttpRequest {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Even the query of an otherwise public URL may carry provider-required credentials.
        f.debug_struct("HttpRequest")
            .field("method", &self.method)
            .field("url", &"[REDACTED]")
            .field("header_count", &self.headers.len())
            .field("has_body", &self.body.is_some())
            .finish_non_exhaustive()
    }
}

pub struct HttpResponse {
    pub status_code: u16,
    pub headers: Vec<HttpHeader>,
    pub body: SecretValue,
}
impl fmt::Debug for HttpResponse {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("HttpResponse")
            .field("status_code", &self.status_code)
            .field("header_count", &self.headers.len())
            .field("body", &"[REDACTED]")
            .finish()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HttpErrorCode {
    InvalidRequest,
    BodyReadFailed,
    Cancelled,
    Timeout,
    NameResolutionFailed,
    ConnectionFailed,
    TlsFailed,
    SendFailed,
    ReceiveFailed,
    ResponseTooLarge,
    ProtocolError,
    UnsupportedEnvironment,
    InitializationFailed,
    Internal,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HttpError {
    pub code: HttpErrorCode,
    pub transport_code: Option<i64>,
    pub body_error: Option<HttpBodyErrorCode>,
    pub system_error: Option<u32>,
}
impl HttpError {
    pub const fn new(code: HttpErrorCode) -> Self {
        Self {
            code,
            transport_code: None,
            body_error: None,
            system_error: None,
        }
    }
}
impl fmt::Display for HttpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for HttpError {}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum HttpTransportPolicy {
    #[default]
    HttpsOnly,
    PlaintextLoopbackForTests,
}

/// Blocking provider-worker boundary. Never execute requests from a render callback.
pub trait HttpClient: Send + Sync {
    fn execute(&self, request: HttpRequest, stop: &StopToken) -> Result<HttpResponse, HttpError>;
}

pub fn validate_http_request(
    request: &HttpRequest,
    policy: HttpTransportPolicy,
) -> Result<(), HttpError> {
    let invalid = || HttpError::new(HttpErrorCode::InvalidRequest);
    validate_http_url(&request.url, policy)?;
    if request.headers.len() > MAX_HTTP_HEADER_COUNT {
        return Err(invalid());
    }
    let mut total = 0;
    for header in &request.headers {
        if header.name.is_empty()
            || header.name.len() > MAX_HTTP_HEADER_NAME_BYTES
            || !header.name.bytes().all(is_http_token_byte)
            || header.value.len() > MAX_HTTP_HEADER_VALUE_BYTES
            || !header
                .value
                .expose()
                .iter()
                .all(|&b| b == b'\t' || (0x20..=0x7e).contains(&b))
            || [
                "Host",
                "Content-Length",
                "Transfer-Encoding",
                "Connection",
                "Expect",
            ]
            .iter()
            .any(|name| header.name.eq_ignore_ascii_case(name))
            || (is_sensitive_http_header_name(&header.name)
                && header.sensitivity != HttpHeaderSensitivity::Sensitive)
        {
            return Err(invalid());
        }
        let length = header.name.len() + header.value.len() + 4;
        if length > MAX_HTTP_REQUEST_HEADER_BYTES - total {
            return Err(invalid());
        }
        total += length;
    }
    if let Some(body) = &request.body {
        let length = catch_unwind(AssertUnwindSafe(|| body.content_length()))
            .map_err(|_| HttpError::new(HttpErrorCode::Internal))?;
        if request.method == HttpMethod::Get || !(1..=MAX_HTTP_REQUEST_BODY_BYTES).contains(&length)
        {
            return Err(invalid());
        }
    }
    let timeouts = request.timeouts;
    if timeouts.connect < Duration::from_millis(1)
        || timeouts.connect > Duration::from_secs(60)
        || timeouts.operation < Duration::from_millis(1)
        || timeouts.operation > Duration::from_secs(900)
        || timeouts.stalled_transfer < Duration::from_secs(1)
        || timeouts.stalled_transfer > Duration::from_secs(900)
        || !(1..=MAX_HTTP_RESPONSE_HEADER_BYTES).contains(&request.response_limits.max_header_bytes)
        || !(1..=MAX_HTTP_RESPONSE_BODY_BYTES).contains(&request.response_limits.max_body_bytes)
    {
        return Err(invalid());
    }
    Ok(())
}

pub fn validate_http_url(url: &str, policy: HttpTransportPolicy) -> Result<(), HttpError> {
    let invalid = || HttpError::new(HttpErrorCode::InvalidRequest);
    if url.is_empty()
        || url.len() > MAX_HTTP_URL_BYTES
        || !url
            .bytes()
            .all(|b| (0x21..=0x7e).contains(&b) && !matches!(b, b'#' | b'\\' | b'"'))
    {
        return Err(invalid());
    }
    let (scheme, rest) = url.split_once("://").ok_or_else(invalid)?;
    let authority = rest.split(['/', '?']).next().ok_or_else(invalid)?;
    if authority.is_empty() || authority.contains('@') {
        return Err(invalid());
    }
    let (host, port) = if authority.starts_with('[') {
        let end = authority.find(']').ok_or_else(invalid)?;
        let host = &authority[..=end];
        authority[1..end]
            .parse::<std::net::Ipv6Addr>()
            .map_err(|_| invalid())?;
        (host, &authority[end + 1..])
    } else {
        let (host, port) = authority.find(':').map_or((authority, ""), |index| {
            (&authority[..index], &authority[index..])
        });
        let name = host.strip_suffix('.').unwrap_or(host);
        if name.is_empty()
            || host.len() > 253
            || name.split('.').any(|label| {
                label.is_empty()
                    || label.len() > 63
                    || label.starts_with('-')
                    || label.ends_with('-')
                    || !label
                        .bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b == b'-')
            })
        {
            return Err(invalid());
        }
        (host, port)
    };
    if !port.is_empty() {
        let digits = port.strip_prefix(':').ok_or_else(invalid)?;
        if digits.is_empty()
            || !digits.bytes().all(|b| b.is_ascii_digit())
            || !digits.parse::<u16>().is_ok_and(|port| port != 0)
        {
            return Err(invalid());
        }
    }
    if scheme.eq_ignore_ascii_case("https") {
        return Ok(());
    }
    if scheme.eq_ignore_ascii_case("http")
        && policy == HttpTransportPolicy::PlaintextLoopbackForTests
        && (host.eq_ignore_ascii_case("localhost") || matches!(host, "127.0.0.1" | "[::1]"))
    {
        return Ok(());
    }
    Err(invalid())
}
