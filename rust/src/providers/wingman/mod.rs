mod response;
mod worker;
pub use worker::WingmanProviderWorker;

use crate::application::*;
use crate::domain::{EncounterMetadata, LogFileIdentity, WVW_BOSS_ID};
use crate::http::{FileBodySource, MemoryBodySource, MultipartFormData, MultipartPart};
use crate::providers::wire::{percent_encode, retry_after, trusted_report_permalink};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;
use std::time::{Duration, Instant};

pub const WINGMAN_UPLOAD_URL: &str = "https://evtc.bel.st/evtc";
pub const WINGMAN_TICKET_URL: &str = "https://evtc.bel.st/status/";
pub const WINGMAN_CHECK_UPLOAD_URL: &str =
    "https://gw2wingman.nevermindcreations.de/checkUploadSuccessfulWithLog";
pub const WINGMAN_LOG_URL: &str = "https://gw2wingman.nevermindcreations.de/log/";
pub const WINGMAN_IMPORT_URL: &str =
    "https://gw2wingman.nevermindcreations.de/api/importLogQueued?link=";
pub const WINGMAN_CHECK_IMPORT_URL: &str =
    "https://gw2wingman.nevermindcreations.de/api/checkLogQueuedOrDB?link=";
pub const MAX_WINGMAN_RESPONSE_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WingmanDisposition {
    Retry,
    Failed,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WingmanErrorCode {
    InvalidConfiguration,
    InvalidInput,
    FileUnavailable,
    MultipartFailed,
    Transport,
    HttpStatus,
    InvalidResponse,
    Declined,
    TicketFailed,
    Internal,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WingmanError {
    pub code: WingmanErrorCode,
    pub disposition: WingmanDisposition,
    pub retry_after: Option<Duration>,
    pub http_error: Option<HttpErrorCode>,
    pub http_status: Option<u16>,
}
impl WingmanError {
    pub const fn failed(code: WingmanErrorCode) -> Self {
        Self {
            code,
            disposition: WingmanDisposition::Failed,
            retry_after: None,
            http_error: None,
            http_status: None,
        }
    }
    fn cancelled() -> Self {
        Self {
            disposition: WingmanDisposition::Cancelled,
            ..Self::failed(WingmanErrorCode::Cancelled)
        }
    }
}
impl std::fmt::Display for WingmanError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for WingmanError {}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WingmanSuccess {
    pub duplicate: bool,
    pub permalink: Option<String>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WingmanPollingOptions {
    pub ticket_interval: Duration,
    pub check_interval: Duration,
    pub ticket_timeout: Duration,
    pub check_timeout: Duration,
}
impl Default for WingmanPollingOptions {
    fn default() -> Self {
        Self {
            ticket_interval: Duration::from_secs(3),
            check_interval: Duration::from_secs(5),
            ticket_timeout: Duration::from_secs(1800),
            check_timeout: Duration::from_secs(300),
        }
    }
}
pub trait WingmanUploader: Send + Sync {
    fn upload(
        &self,
        file: &LogFileIdentity,
        metadata: &EncounterMetadata,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError>;
    fn import_permalink(
        &self,
        permalink: &str,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError>;
}
pub struct WingmanClient {
    http: Arc<dyn HttpClient>,
    random: Arc<dyn RandomSource>,
    clock: Arc<dyn PollClock>,
    upload_url: String,
    polling: WingmanPollingOptions,
}
impl WingmanClient {
    pub fn new(http: Arc<dyn HttpClient>, random: Arc<dyn RandomSource>) -> Self {
        Self {
            http,
            random,
            clock: Arc::new(SystemPollClock),
            upload_url: WINGMAN_UPLOAD_URL.into(),
            polling: WingmanPollingOptions::default(),
        }
    }
    pub fn with_options(
        http: Arc<dyn HttpClient>,
        random: Arc<dyn RandomSource>,
        clock: Arc<dyn PollClock>,
        upload_url: String,
        polling: WingmanPollingOptions,
    ) -> Result<Self, WingmanError> {
        if upload_url.len() > 2048
            || upload_url.contains('@')
            || validate_http_url(&upload_url, HttpTransportPolicy::HttpsOnly).is_err()
            || polling.ticket_interval.is_zero()
            || polling.check_interval.is_zero()
            || polling.ticket_interval > Duration::from_secs(1800)
            || polling.check_interval > Duration::from_secs(300)
            || polling.ticket_timeout > Duration::from_secs(1800)
            || polling.check_timeout > Duration::from_secs(300)
        {
            return Err(WingmanError::failed(WingmanErrorCode::InvalidConfiguration));
        }
        Ok(Self {
            http,
            random,
            clock,
            upload_url,
            polling,
        })
    }
    fn execute(
        &self,
        request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, WingmanError> {
        if stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        let result = self.http.execute(request, stop);
        if stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        result.map_err(classify_transport)
    }
    fn upload_inner(
        &self,
        file: &LogFileIdentity,
        metadata: &EncounterMetadata,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError> {
        if stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        if file.size == 0
            || metadata.boss_id == 0
            || metadata.boss_id == WVW_BOSS_ID
            || metadata.pov_account.is_empty()
            || metadata.pov_account.len() > 256
            || metadata.pov_account.bytes().any(|b| b < 0x20 || b == 0x7f)
        {
            return Err(WingmanError::failed(WingmanErrorCode::InvalidInput));
        }
        let file_body = FileBodySource::new(file.clone())
            .map_err(|_| WingmanError::failed(WingmanErrorCode::FileUnavailable))?;
        let mut parts = Vec::with_capacity(4);
        for (name, value) in [
            ("account", metadata.pov_account.clone()),
            ("filesize", file.size.to_string()),
            ("triggerID", metadata.boss_id.to_string()),
        ] {
            parts.push(MultipartPart {
                name: name.into(),
                filename: None,
                content_type: Some("text/plain".into()),
                body: Box::new(memory(value)?),
            });
        }
        parts.push(MultipartPart {
            name: "file".into(),
            filename: Some("upload.zevtc".into()),
            content_type: Some("application/octet-stream".into()),
            body: Box::new(file_body),
        });
        let form = MultipartFormData::new(parts, self.random.as_ref())
            .map_err(|_| WingmanError::failed(WingmanErrorCode::MultipartFailed))?;
        let mut request = json_request(HttpMethod::Post, self.upload_url.clone());
        request
            .headers
            .push(HttpHeader::public("Content-Type", form.content_type));
        request.body = Some(form.body);
        request.timeouts.operation = Duration::from_secs(900);
        request.timeouts.stalled_transfer = Duration::from_secs(900);
        let reply = self.execute(request, stop)?;
        let duplicate = reply.status_code == 409;
        if !duplicate {
            require_success(&reply)?;
            let accepted: response::Accepted = response::decode(&reply)?;
            if !accepted.result {
                return Err(WingmanError::failed(WingmanErrorCode::Declined));
            }
            match accepted.ticket {
                None => {
                    return Ok(WingmanSuccess {
                        duplicate: false,
                        permalink: None,
                    });
                }
                Some(0) => return Err(WingmanError::failed(WingmanErrorCode::InvalidResponse)),
                Some(ticket) => self.poll_ticket(ticket, stop)?,
            }
        }
        let permalink = self.find_permalink(file, metadata, stop)?;
        Ok(WingmanSuccess {
            duplicate,
            permalink,
        })
    }
    fn deadline(&self, duration: Duration) -> Result<Instant, WingmanError> {
        self.clock
            .now()
            .checked_add(duration)
            .ok_or(WingmanError::failed(WingmanErrorCode::Internal))
    }
    fn wait(
        &self,
        deadline: Instant,
        interval: Duration,
        stop: &StopToken,
    ) -> Result<bool, WingmanError> {
        if stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        let remaining = deadline.saturating_duration_since(self.clock.now());
        if remaining.is_zero() {
            return Ok(false);
        }
        if self.clock.wait(interval.min(remaining), stop) || stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        Ok(self.clock.now() < deadline)
    }
    fn poll_ticket(&self, ticket: u64, stop: &StopToken) -> Result<(), WingmanError> {
        let deadline = self.deadline(self.polling.ticket_timeout)?;
        while self.wait(deadline, self.polling.ticket_interval, stop)? {
            let Some(request) = self.poll_request(
                HttpMethod::Get,
                format!("{WINGMAN_TICKET_URL}{ticket}"),
                deadline,
            ) else {
                break;
            };
            let reply = self.execute(request, stop)?;
            if reply.status_code == 404 {
                return Ok(());
            }
            require_success(&reply)?;
            let status: response::Ticket = response::decode(&reply)?;
            match status.state.0.as_str() {
                "uploaded" | "skipped" => return Ok(()),
                "queued" | "processing" | "deferred" => {}
                "failed" => return Err(WingmanError::failed(WingmanErrorCode::TicketFailed)),
                _ => return Err(WingmanError::failed(WingmanErrorCode::InvalidResponse)),
            }
        }
        Ok(())
    }
    fn find_permalink(
        &self,
        file: &LogFileIdentity,
        metadata: &EncounterMetadata,
        stop: &StopToken,
    ) -> Result<Option<String>, WingmanError> {
        let deadline = self.deadline(self.polling.check_timeout)?;
        let remote_name = format!("{}_upload.zevtc", metadata.pov_account.replace('.', ""));
        while self.clock.now() < deadline {
            let text = format!(
                "file={}&filesize={}&bossID={}&account={}",
                percent_encode(&remote_name),
                file.size,
                metadata.boss_id,
                percent_encode(&metadata.pov_account)
            );
            let Some(mut request) =
                self.poll_request(HttpMethod::Post, WINGMAN_CHECK_UPLOAD_URL.into(), deadline)
            else {
                break;
            };
            request.headers.push(HttpHeader::public(
                "Content-Type",
                "application/x-www-form-urlencoded",
            ));
            request.body = Some(Box::new(memory(text)?));
            let reply = self.execute(request, stop)?;
            require_success(&reply)?;
            let found: response::Found = response::decode(&reply)?;
            if found.success {
                let slug = found
                    .log
                    .and_then(|log| log.0.html)
                    .ok_or(WingmanError::failed(WingmanErrorCode::InvalidResponse))?
                    .0;
                if !response::safe_slug(&slug) {
                    return Err(WingmanError::failed(WingmanErrorCode::InvalidResponse));
                }
                return Ok(Some(format!("{WINGMAN_LOG_URL}{slug}")));
            }
            if !self.wait(deadline, self.polling.check_interval, stop)? {
                break;
            }
        }
        if stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        Ok(None)
    }
    fn poll_request(
        &self,
        method: HttpMethod,
        url: String,
        deadline: Instant,
    ) -> Option<HttpRequest> {
        let remaining = deadline.saturating_duration_since(self.clock.now());
        if remaining < Duration::from_millis(1) {
            return None;
        }
        let mut request = json_request(method, url);
        request.timeouts.operation = request.timeouts.operation.min(remaining);
        Some(request)
    }
    fn import_inner(
        &self,
        permalink: &str,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError> {
        if stop.is_stop_requested() {
            return Err(WingmanError::cancelled());
        }
        if !trusted_report_permalink(permalink) {
            return Err(WingmanError::failed(WingmanErrorCode::InvalidInput));
        }
        let encoded = percent_encode(permalink);
        if encoded.len() + WINGMAN_IMPORT_URL.len().max(WINGMAN_CHECK_IMPORT_URL.len())
            > MAX_HTTP_URL_BYTES
        {
            return Err(WingmanError::failed(WingmanErrorCode::InvalidInput));
        }
        let queued = self.execute(
            json_request(HttpMethod::Post, format!("{WINGMAN_IMPORT_URL}{encoded}")),
            stop,
        )?;
        require_success(&queued)?;
        let queued: response::Queued = response::decode(&queued)?;
        if !matches!(queued.success, 0 | 1) {
            return Err(WingmanError::failed(WingmanErrorCode::InvalidResponse));
        }
        let checked = self.execute(
            json_request(
                HttpMethod::Get,
                format!("{WINGMAN_CHECK_IMPORT_URL}{encoded}"),
            ),
            stop,
        )?;
        require_success(&checked)?;
        let checked: response::Checked = response::decode(&checked)?;
        if !(queued.success == 1 || checked.in_queue || checked.in_db)
            || !checked
                .target_url
                .0
                .strip_prefix(WINGMAN_LOG_URL)
                .is_some_and(response::safe_slug)
        {
            return Err(WingmanError::failed(WingmanErrorCode::InvalidResponse));
        }
        Ok(WingmanSuccess {
            duplicate: checked.in_db,
            permalink: Some(checked.target_url.0),
        })
    }
}
impl WingmanUploader for WingmanClient {
    fn upload(
        &self,
        file: &LogFileIdentity,
        metadata: &EncounterMetadata,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError> {
        catch_unwind(AssertUnwindSafe(|| self.upload_inner(file, metadata, stop)))
            .unwrap_or(Err(WingmanError::failed(WingmanErrorCode::Internal)))
    }
    fn import_permalink(
        &self,
        permalink: &str,
        stop: &StopToken,
    ) -> Result<WingmanSuccess, WingmanError> {
        catch_unwind(AssertUnwindSafe(|| self.import_inner(permalink, stop)))
            .unwrap_or(Err(WingmanError::failed(WingmanErrorCode::Internal)))
    }
}
fn memory(value: String) -> Result<MemoryBodySource, WingmanError> {
    MemoryBodySource::new(value.into_bytes())
        .map_err(|_| WingmanError::failed(WingmanErrorCode::MultipartFailed))
}
fn json_request(method: HttpMethod, url: String) -> HttpRequest {
    let mut request = HttpRequest::new(method, url);
    request
        .headers
        .push(HttpHeader::public("Accept", "application/json"));
    request.timeouts = HttpTimeouts {
        connect: Duration::from_secs(10),
        operation: Duration::from_secs(60),
        stalled_transfer: Duration::from_secs(30),
    };
    request.response_limits = HttpResponseLimits {
        max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
        max_body_bytes: MAX_WINGMAN_RESPONSE_BYTES,
    };
    request
}
fn require_success(reply: &HttpResponse) -> Result<(), WingmanError> {
    let status = reply.status_code;
    if (200..300).contains(&status) {
        return Ok(());
    }
    let retry = status == 408 || status == 429 || (500..600).contains(&status);
    Err(WingmanError {
        code: WingmanErrorCode::HttpStatus,
        disposition: if retry {
            WingmanDisposition::Retry
        } else {
            WingmanDisposition::Failed
        },
        retry_after: retry.then_some(Duration::from_secs(if status == 429 {
            retry_after(reply).unwrap_or(60)
        } else {
            30
        })),
        http_error: None,
        http_status: Some(status),
    })
}
fn classify_transport(error: HttpError) -> WingmanError {
    let retry = matches!(
        error.code,
        HttpErrorCode::Timeout
            | HttpErrorCode::NameResolutionFailed
            | HttpErrorCode::ConnectionFailed
            | HttpErrorCode::TlsFailed
            | HttpErrorCode::SendFailed
            | HttpErrorCode::ReceiveFailed
    );
    WingmanError {
        code: WingmanErrorCode::Transport,
        disposition: if error.code == HttpErrorCode::Cancelled {
            WingmanDisposition::Cancelled
        } else if retry {
            WingmanDisposition::Retry
        } else {
            WingmanDisposition::Failed
        },
        retry_after: retry.then_some(Duration::from_secs(30)),
        http_error: Some(error.code),
        http_status: None,
    }
}
