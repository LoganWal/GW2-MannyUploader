mod upload;
mod upload_response;
mod verification;
mod verification_worker;
pub use upload::{DonBotAggregateSuccess, DonBotOptions, DonBotUploadSuccess, DonBotUploader};
pub use verification_worker::DonBotVerificationWorker;

use crate::application::*;
use crate::http::MemoryBodySource;
use crate::providers::wire::retry_after;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::time::Duration;

pub const MAX_DONBOT_VERIFICATION_BYTES: usize = 256 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotDisposition {
    Retry,
    Failed,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DonBotErrorCode {
    InvalidBase,
    InvalidKey,
    InvalidInput,
    BodyFailed,
    FileUnavailable,
    InvalidLocation,
    UnconfirmedUpload,
    ProcessingFailed,
    UnconfirmedDelivery,
    Transport,
    HttpStatus,
    InvalidResponse,
    Internal,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DonBotError {
    pub code: DonBotErrorCode,
    pub disposition: DonBotDisposition,
    pub retry_after: Option<Duration>,
    pub http_error: Option<HttpErrorCode>,
    pub http_status: Option<u16>,
}
impl DonBotError {
    pub const fn failed(code: DonBotErrorCode) -> Self {
        Self {
            code,
            disposition: DonBotDisposition::Failed,
            retry_after: None,
            http_error: None,
            http_status: None,
        }
    }
    fn cancelled() -> Self {
        Self {
            disposition: DonBotDisposition::Cancelled,
            ..Self::failed(DonBotErrorCode::Cancelled)
        }
    }
}
impl std::fmt::Display for DonBotError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for DonBotError {}

pub trait DonBotIdentityClient: Send + Sync {
    fn verify(
        &self,
        api_base: &str,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotVerification, DonBotError>;
}
pub struct DonBotClient {
    http: Arc<dyn HttpClient>,
}
impl DonBotClient {
    pub fn new(http: Arc<dyn HttpClient>) -> Self {
        Self { http }
    }
    fn verify_inner(
        &self,
        api_base: &str,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotVerification, DonBotError> {
        if stop.is_stop_requested() {
            return Err(DonBotError::cancelled());
        }
        let base = normalize_donbot_api_base(api_base)
            .ok_or(DonBotError::failed(DonBotErrorCode::InvalidBase))?;
        if !valid_donbot_api_key(key) {
            return Err(DonBotError::failed(DonBotErrorCode::InvalidKey));
        }
        // Allocate once before copying the key so growth cannot leave unwiped secret allocations.
        let mut body = SecretValue::from_bytes(vec![0; key.len() + 13]);
        let bytes = body.expose_mut();
        bytes[..11].copy_from_slice(b"{\"apiKey\":\"");
        bytes[11..11 + key.len()].copy_from_slice(key.expose());
        bytes[11 + key.len()..].copy_from_slice(b"\"}");
        let mut request =
            HttpRequest::new(HttpMethod::Post, format!("{base}/api/upload/gw2/guilds"));
        request.headers = vec![
            HttpHeader::public("Accept", "application/json"),
            HttpHeader::public("Content-Type", "application/json"),
        ];
        request.body = Some(Box::new(
            MemoryBodySource::from_secret(body)
                .map_err(|_| DonBotError::failed(DonBotErrorCode::BodyFailed))?,
        ));
        request.timeouts = HttpTimeouts {
            connect: Duration::from_secs(5),
            operation: Duration::from_secs(30),
            stalled_transfer: Duration::from_secs(30),
        };
        request.response_limits = HttpResponseLimits {
            max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
            max_body_bytes: MAX_DONBOT_VERIFICATION_BYTES,
        };
        let response = self.http.execute(request, stop);
        if stop.is_stop_requested() {
            return Err(DonBotError::cancelled());
        }
        let response = response.map_err(classify_transport)?;
        if response.status_code != 200 {
            return Err(classify_status(&response));
        }
        verification::decode(&response)
    }
}
impl DonBotIdentityClient for DonBotClient {
    fn verify(
        &self,
        api_base: &str,
        key: &SecretValue,
        stop: &StopToken,
    ) -> Result<DonBotVerification, DonBotError> {
        catch_unwind(AssertUnwindSafe(|| self.verify_inner(api_base, key, stop)))
            .unwrap_or(Err(DonBotError::failed(DonBotErrorCode::Internal)))
    }
}
fn classify_status(response: &HttpResponse) -> DonBotError {
    let status = response.status_code;
    let retry = status == 408 || status == 429 || (500..600).contains(&status);
    DonBotError {
        code: DonBotErrorCode::HttpStatus,
        disposition: if retry {
            DonBotDisposition::Retry
        } else {
            DonBotDisposition::Failed
        },
        retry_after: retry.then_some(Duration::from_secs(if status == 429 {
            retry_after(response).unwrap_or(60)
        } else {
            30
        })),
        http_status: Some(status),
        http_error: None,
    }
}
fn classify_transport(error: HttpError) -> DonBotError {
    let retry = matches!(
        error.code,
        HttpErrorCode::Timeout
            | HttpErrorCode::NameResolutionFailed
            | HttpErrorCode::ConnectionFailed
            | HttpErrorCode::TlsFailed
            | HttpErrorCode::SendFailed
            | HttpErrorCode::ReceiveFailed
    );
    DonBotError {
        code: DonBotErrorCode::Transport,
        disposition: if error.code == HttpErrorCode::Cancelled {
            DonBotDisposition::Cancelled
        } else if retry {
            DonBotDisposition::Retry
        } else {
            DonBotDisposition::Failed
        },
        retry_after: retry.then_some(Duration::from_secs(30)),
        http_status: None,
        http_error: Some(error.code),
    }
}
