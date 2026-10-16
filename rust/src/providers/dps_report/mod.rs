mod response;
mod worker;
pub use worker::DpsReportProviderWorker;

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::time::Duration;

use crate::application::*;
use crate::domain::{DpsReportResult, LogFileIdentity};
use crate::http::{FileBodySource, MemoryBodySource, MultipartFormData, MultipartPart};

pub const DPS_REPORT_UPLOAD_URL: &str = "https://dps.report/uploadContent?json=1&generator=ei";
pub const DPS_REPORT_DETAILED_WVW_URL: &str =
    "https://dps.report/uploadContent?json=1&generator=ei&detailedwvw=true";
pub const MAX_DPS_REPORT_TOKEN_BYTES: usize = 256;
pub const MAX_DPS_REPORT_RESPONSE_BYTES: usize = 1024 * 1024;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DpsReportOptions {
    pub detailed_wvw: bool,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DpsReportDisposition {
    Retry,
    Failed,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DpsReportErrorCode {
    InvalidToken,
    FileUnavailable,
    MultipartFailed,
    Transport,
    HttpStatus,
    InvalidResponse,
    Internal,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DpsReportError {
    pub code: DpsReportErrorCode,
    pub disposition: DpsReportDisposition,
    pub retry_after: Option<Duration>,
    pub http_error: Option<HttpErrorCode>,
    pub http_status: Option<u16>,
}
impl DpsReportError {
    pub const fn failed(code: DpsReportErrorCode) -> Self {
        Self {
            code,
            disposition: DpsReportDisposition::Failed,
            retry_after: None,
            http_error: None,
            http_status: None,
        }
    }
    fn cancelled() -> Self {
        Self {
            disposition: DpsReportDisposition::Cancelled,
            ..Self::failed(DpsReportErrorCode::Cancelled)
        }
    }
}
impl std::fmt::Display for DpsReportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for DpsReportError {}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DpsReportWarning {
    GeneratedWithWarning,
    InvalidReturnedToken,
}
#[derive(Debug)]
pub struct DpsReportSuccess {
    pub report: DpsReportResult,
    pub replacement_user_token: Option<SecretValue>,
    pub warning: Option<DpsReportWarning>,
}
pub trait DpsReportUploader: Send + Sync {
    fn upload(
        &self,
        file: &LogFileIdentity,
        token: Option<&SecretValue>,
        options: DpsReportOptions,
        stop: &StopToken,
    ) -> Result<DpsReportSuccess, DpsReportError>;
}
pub struct DpsReportClient {
    http: Arc<dyn HttpClient>,
    random: Arc<dyn RandomSource>,
}
impl DpsReportClient {
    pub fn new(http: Arc<dyn HttpClient>, random: Arc<dyn RandomSource>) -> Self {
        Self { http, random }
    }
    fn upload_inner(
        &self,
        file: &LogFileIdentity,
        token: Option<&SecretValue>,
        options: DpsReportOptions,
        stop: &StopToken,
    ) -> Result<DpsReportSuccess, DpsReportError> {
        if stop.is_stop_requested() {
            return Err(DpsReportError::cancelled());
        }
        if token.is_some_and(|token| token.is_empty() || !valid_token(token.expose())) {
            return Err(DpsReportError::failed(DpsReportErrorCode::InvalidToken));
        }
        if file.size == 0 {
            return Err(DpsReportError::failed(DpsReportErrorCode::FileUnavailable));
        }
        let source = FileBodySource::new(file.clone())
            .map_err(|_| DpsReportError::failed(DpsReportErrorCode::FileUnavailable))?;
        let mut parts = Vec::with_capacity(2);
        if let Some(token) = token {
            let owned = SecretValue::from_bytes(token.expose().to_vec());
            let body = MemoryBodySource::from_secret(owned)
                .map_err(|_| DpsReportError::failed(DpsReportErrorCode::InvalidToken))?;
            parts.push(MultipartPart {
                name: "userToken".into(),
                filename: None,
                content_type: Some("text/plain".into()),
                body: Box::new(body),
            });
        }
        parts.push(MultipartPart {
            name: "file".into(),
            filename: Some("upload.zevtc".into()),
            content_type: Some("application/octet-stream".into()),
            body: Box::new(source),
        });
        let form = MultipartFormData::new(parts, self.random.as_ref())
            .map_err(|_| DpsReportError::failed(DpsReportErrorCode::MultipartFailed))?;
        let mut request = HttpRequest::new(
            HttpMethod::Post,
            if options.detailed_wvw {
                DPS_REPORT_DETAILED_WVW_URL
            } else {
                DPS_REPORT_UPLOAD_URL
            },
        );
        request.headers = vec![
            HttpHeader::public("Accept", "application/json"),
            HttpHeader::public("Content-Type", form.content_type),
        ];
        request.body = Some(form.body);
        request.timeouts = HttpTimeouts {
            connect: Duration::from_secs(10),
            operation: Duration::from_secs(900),
            stalled_transfer: Duration::from_secs(900),
        };
        request.response_limits = HttpResponseLimits {
            max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
            max_body_bytes: MAX_DPS_REPORT_RESPONSE_BYTES,
        };
        if stop.is_stop_requested() {
            return Err(DpsReportError::cancelled());
        }
        let result = self.http.execute(request, stop);
        if stop.is_stop_requested() {
            return Err(DpsReportError::cancelled());
        }
        let response = result.map_err(classify_transport)?;
        if (200..300).contains(&response.status_code) {
            response::decode(response, token)
        } else {
            Err(classify_status(&response))
        }
    }
}
impl DpsReportUploader for DpsReportClient {
    fn upload(
        &self,
        file: &LogFileIdentity,
        token: Option<&SecretValue>,
        options: DpsReportOptions,
        stop: &StopToken,
    ) -> Result<DpsReportSuccess, DpsReportError> {
        catch_unwind(AssertUnwindSafe(|| {
            self.upload_inner(file, token, options, stop)
        }))
        .unwrap_or(Err(DpsReportError::failed(DpsReportErrorCode::Internal)))
    }
}
fn valid_token(bytes: &[u8]) -> bool {
    bytes.len() <= MAX_DPS_REPORT_TOKEN_BYTES && bytes.iter().all(|&b| (0x21..=0x7e).contains(&b))
}
fn classify_transport(error: HttpError) -> DpsReportError {
    let retry = matches!(
        error.code,
        HttpErrorCode::Timeout
            | HttpErrorCode::NameResolutionFailed
            | HttpErrorCode::ConnectionFailed
            | HttpErrorCode::TlsFailed
            | HttpErrorCode::SendFailed
            | HttpErrorCode::ReceiveFailed
    );
    DpsReportError {
        code: DpsReportErrorCode::Transport,
        disposition: if error.code == HttpErrorCode::Cancelled {
            DpsReportDisposition::Cancelled
        } else if retry {
            DpsReportDisposition::Retry
        } else {
            DpsReportDisposition::Failed
        },
        retry_after: retry.then_some(Duration::from_secs(30)),
        http_error: Some(error.code),
        http_status: None,
    }
}
fn classify_status(response: &HttpResponse) -> DpsReportError {
    let status = response.status_code;
    let retry = status == 408 || status == 429 || (500..600).contains(&status);
    let delay = if status == 429 {
        retry_after(response).unwrap_or(60)
    } else {
        30
    };
    DpsReportError {
        code: DpsReportErrorCode::HttpStatus,
        disposition: if retry {
            DpsReportDisposition::Retry
        } else {
            DpsReportDisposition::Failed
        },
        retry_after: retry.then_some(Duration::from_secs(delay)),
        http_error: None,
        http_status: Some(status),
    }
}
fn retry_after(response: &HttpResponse) -> Option<u64> {
    let mut matching = response
        .headers
        .iter()
        .filter(|header| header.name().eq_ignore_ascii_case("Retry-After"));
    let bytes = matching.next()?.expose_value();
    if matching.next().is_some() || bytes.is_empty() || !bytes.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let seconds = bytes.iter().try_fold(0u64, |total, &byte| {
        total.checked_mul(10)?.checked_add(u64::from(byte - b'0'))
    })?;
    (1..=900).contains(&seconds).then_some(seconds)
}
