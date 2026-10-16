mod authentication_worker;
mod response;
pub use authentication_worker::TwitchAuthenticationWorker;

use super::wire::retry_after;
use crate::{application::*, domain::TwitchDeliveryReceipt, http::MemoryBodySource};
use std::{
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{Arc, Mutex},
    time::Duration,
};

const DEVICE_URL: &str = "https://id.twitch.tv/oauth2/device";
const TOKEN_URL: &str = "https://id.twitch.tv/oauth2/token";
const VALIDATE_URL: &str = "https://id.twitch.tv/oauth2/validate";
const REVOKE_URL: &str = "https://id.twitch.tv/oauth2/revoke";
const CHAT_URL: &str = "https://api.twitch.tv/helix/chat/messages";
const DEVICE_GRANT: &[u8] = b"urn:ietf:params:oauth:grant-type:device_code";
const MAX_RESPONSE_BYTES: usize = 64 * 1024;

pub struct TwitchClient {
    http: Arc<dyn HttpClient>,
    client_id: Mutex<String>,
}
impl TwitchClient {
    pub fn new(http: Arc<dyn HttpClient>, client_id: String) -> Result<Self, TwitchError> {
        if !valid_twitch_client_id(&client_id) {
            return Err(TwitchError::failed(
                TwitchOperation::Start,
                TwitchErrorCode::InvalidClientId,
            ));
        }
        Ok(Self {
            http,
            client_id: Mutex::new(client_id),
        })
    }
    pub fn unconfigured(http: Arc<dyn HttpClient>) -> Self {
        Self {
            http,
            client_id: Mutex::new(String::new()),
        }
    }
    /// The application owner permits replacement only while disconnected or in error.
    pub fn update_client_id(&self, client_id: String) -> Result<(), TwitchError> {
        if !client_id.is_empty() && !valid_twitch_client_id(&client_id) {
            return Err(TwitchError::failed(
                TwitchOperation::Start,
                TwitchErrorCode::InvalidClientId,
            ));
        }
        *self.client_id.lock().map_err(|_| {
            TwitchError::failed(TwitchOperation::Start, TwitchErrorCode::Internal)
        })? = client_id;
        Ok(())
    }
    pub fn configured(&self) -> bool {
        self.client_id.lock().is_ok_and(|id| !id.is_empty())
    }

    fn run<T>(
        &self,
        operation: TwitchOperation,
        stop: &StopToken,
        call: impl FnOnce(&str) -> Result<T, TwitchError>,
    ) -> Result<T, TwitchError> {
        catch_unwind(AssertUnwindSafe(|| {
            if stop.is_stop_requested() {
                return Err(cancelled(operation));
            }
            let id = self
                .client_id
                .lock()
                .map_err(|_| TwitchError::failed(operation, TwitchErrorCode::Internal))?
                .clone();
            if id.is_empty() {
                return Err(TwitchError::failed(
                    operation,
                    TwitchErrorCode::Unconfigured,
                ));
            }
            call(&id)
        }))
        .unwrap_or(Err(TwitchError::failed(
            operation,
            TwitchErrorCode::Internal,
        )))
    }
    fn execute(
        &self,
        mut request: HttpRequest,
        operation: TwitchOperation,
        stop: &StopToken,
    ) -> Result<HttpResponse, TwitchError> {
        request.timeouts = HttpTimeouts {
            connect: Duration::from_secs(10),
            operation: Duration::from_secs(30),
            stalled_transfer: Duration::from_secs(30),
        };
        request.response_limits = HttpResponseLimits {
            max_header_bytes: MAX_HTTP_RESPONSE_HEADER_BYTES,
            max_body_bytes: MAX_RESPONSE_BYTES,
        };
        if stop.is_stop_requested() {
            return Err(cancelled(operation));
        }
        let result = self.http.execute(request, stop);
        if stop.is_stop_requested() {
            return Err(cancelled(operation));
        }
        result.map_err(|error| transport_error(operation, error.code))
    }
    fn post(
        &self,
        url: &str,
        fields: &[(&str, &[u8])],
        operation: TwitchOperation,
        stop: &StopToken,
    ) -> Result<HttpResponse, TwitchError> {
        let mut request = HttpRequest::new(HttpMethod::Post, url);
        request.headers = vec![
            HttpHeader::public("Accept", "application/json"),
            HttpHeader::public("Content-Type", "application/x-www-form-urlencoded"),
        ];
        request.body = Some(Box::new(
            MemoryBodySource::from_secret(form(fields))
                .map_err(|_| TwitchError::failed(operation, TwitchErrorCode::BodyFailed))?,
        ));
        self.execute(request, operation, stop)
    }
}
impl TwitchApi for TwitchClient {
    fn start_device_authorization(
        &self,
        stop: &StopToken,
    ) -> Result<TwitchDeviceAuthorization, TwitchError> {
        let op = TwitchOperation::Start;
        self.run(op, stop, |id| {
            let result = self.post(
                DEVICE_URL,
                &[
                    ("client_id", id.as_bytes()),
                    ("scopes", TWITCH_CHAT_SCOPE.as_bytes()),
                ],
                op,
                stop,
            )?;
            require_success(&result, op)?;
            response::device(&result)
        })
    }
    fn poll_device_authorization(
        &self,
        code: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchDevicePollResult, TwitchError> {
        let op = TwitchOperation::Poll;
        self.run(op, stop, |id| {
            require_token(code, op)?;
            let result = self.post(
                TOKEN_URL,
                &[
                    ("client_id", id.as_bytes()),
                    ("scopes", TWITCH_CHAT_SCOPE.as_bytes()),
                    ("device_code", code.expose()),
                    ("grant_type", DEVICE_GRANT),
                ],
                op,
                stop,
            )?;
            if result.status_code == 400 && response::pending(&result) {
                return Ok(TwitchDevicePollResult::Pending);
            }
            require_success(&result, op)?;
            response::grant(&result, op).map(TwitchDevicePollResult::Granted)
        })
    }
    fn validate_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchValidatedIdentity, TwitchError> {
        let op = TwitchOperation::Validate;
        self.run(op, stop, |id| {
            require_token(token, op)?;
            let mut request = HttpRequest::new(HttpMethod::Get, VALIDATE_URL);
            request.headers = vec![
                HttpHeader::public("Accept", "application/json"),
                authorization(b"OAuth ", token),
            ];
            let result = self.execute(request, op, stop)?;
            require_success(&result, op)?;
            response::identity(&result, id)
        })
    }
    fn refresh_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchTokenGrant, TwitchError> {
        let op = TwitchOperation::Refresh;
        self.run(op, stop, |id| {
            require_token(token, op)?;
            let result = self.post(
                TOKEN_URL,
                &[
                    ("grant_type", b"refresh_token"),
                    ("refresh_token", token.expose()),
                    ("client_id", id.as_bytes()),
                ],
                op,
                stop,
            )?;
            require_success(&result, op)?;
            response::grant(&result, op)
        })
    }
    fn revoke_access_token(
        &self,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<(), TwitchError> {
        let op = TwitchOperation::Revoke;
        self.run(op, stop, |id| {
            require_token(token, op)?;
            let result = self.post(
                REVOKE_URL,
                &[("client_id", id.as_bytes()), ("token", token.expose())],
                op,
                stop,
            )?;
            require_success(&result, op)
        })
    }
    fn send_chat_message(
        &self,
        user_id: &str,
        message: &str,
        token: &SecretValue,
        stop: &StopToken,
    ) -> Result<TwitchDeliveryReceipt, TwitchError> {
        let op = TwitchOperation::SendChat;
        self.run(op, stop, |id| {
            require_token(token, op)?;
            if !valid_twitch_user_id(user_id)
                || message.is_empty()
                || message.len() > 2000
                || message.chars().count() > 500
                || message.bytes().any(|b| b.is_ascii_control())
            {
                return Err(TwitchError::failed(op, TwitchErrorCode::InvalidInput));
            }
            #[derive(serde::Serialize)]
            struct Chat<'a> {
                broadcaster_id: &'a str,
                sender_id: &'a str,
                message: &'a str,
            }
            let body = serde_json::to_vec(&Chat {
                broadcaster_id: user_id,
                sender_id: user_id,
                message,
            })
            .map_err(|_| TwitchError::failed(op, TwitchErrorCode::BodyFailed))?;
            let mut request = HttpRequest::new(HttpMethod::Post, CHAT_URL);
            request.headers = vec![
                HttpHeader::public("Accept", "application/json"),
                HttpHeader::public("Client-Id", id),
                HttpHeader::public("Content-Type", "application/json"),
                authorization(b"Bearer ", token),
            ];
            request.body =
                Some(Box::new(MemoryBodySource::new(body).map_err(|_| {
                    TwitchError::failed(op, TwitchErrorCode::BodyFailed)
                })?));
            let result = self.execute(request, op, stop)?;
            require_success(&result, op)?;
            response::chat(&result)
        })
    }
}

fn require_token(token: &SecretValue, op: TwitchOperation) -> Result<(), TwitchError> {
    if valid_twitch_token(token) {
        return Ok(());
    }
    let mut error = TwitchError::failed(op, TwitchErrorCode::InvalidInput);
    if matches!(op, TwitchOperation::Validate | TwitchOperation::Refresh) {
        error.disposition = TwitchDisposition::Reconnect;
    }
    Err(error)
}
fn cancelled(operation: TwitchOperation) -> TwitchError {
    TwitchError {
        disposition: TwitchDisposition::Cancelled,
        ..TwitchError::failed(operation, TwitchErrorCode::Cancelled)
    }
}
fn transport_error(op: TwitchOperation, code: HttpErrorCode) -> TwitchError {
    let disposition = match code {
        HttpErrorCode::Cancelled => TwitchDisposition::Cancelled,
        HttpErrorCode::Timeout
        | HttpErrorCode::NameResolutionFailed
        | HttpErrorCode::ConnectionFailed
        | HttpErrorCode::TlsFailed
        | HttpErrorCode::SendFailed
        | HttpErrorCode::ReceiveFailed => TwitchDisposition::Retry,
        _ => TwitchDisposition::Failed,
    };
    TwitchError {
        disposition,
        retry_after: (disposition == TwitchDisposition::Retry).then_some(Duration::from_secs(30)),
        http_error: Some(code),
        ..TwitchError::failed(op, TwitchErrorCode::Transport)
    }
}
fn require_success(response: &HttpResponse, op: TwitchOperation) -> Result<(), TwitchError> {
    let status = response.status_code;
    if status == 200 {
        return Ok(());
    }
    let reconnect = (op == TwitchOperation::Refresh && matches!(status, 400 | 401))
        || (matches!(op, TwitchOperation::Validate | TwitchOperation::SendChat) && status == 401);
    let retry = status == 408 || status == 429 || (500..600).contains(&status);
    Err(TwitchError {
        disposition: if reconnect {
            TwitchDisposition::Reconnect
        } else if retry {
            TwitchDisposition::Retry
        } else {
            TwitchDisposition::Failed
        },
        retry_after: retry.then(|| {
            Duration::from_secs(if status == 429 {
                retry_after(response).unwrap_or(60)
            } else {
                30
            })
        }),
        http_status: Some(status),
        ..TwitchError::failed(op, TwitchErrorCode::HttpStatus)
    })
}
fn authorization(prefix: &[u8], token: &SecretValue) -> HttpHeader {
    let mut value = SecretValue::from_bytes(vec![0; prefix.len() + token.len()]);
    value.expose_mut()[..prefix.len()].copy_from_slice(prefix);
    value.expose_mut()[prefix.len()..].copy_from_slice(token.expose());
    HttpHeader::sensitive("Authorization", value)
}
fn form(fields: &[(&str, &[u8])]) -> SecretValue {
    fn unreserved(b: u8) -> bool {
        b.is_ascii_alphanumeric() || b"-_.~".contains(&b)
    }
    let length = fields.len().saturating_sub(1)
        + fields
            .iter()
            .map(|(name, value)| {
                name.len()
                    + 1
                    + value
                        .iter()
                        .map(|&b| if unreserved(b) { 1 } else { 3 })
                        .sum::<usize>()
            })
            .sum::<usize>();
    // All callers supply bounded values and static field names. Never grow a secret allocation.
    let mut result = SecretValue::from_bytes(vec![0; length]);
    let bytes = result.expose_mut();
    let mut cursor = 0;
    for (index, (name, value)) in fields.iter().enumerate() {
        if index != 0 {
            bytes[cursor] = b'&';
            cursor += 1;
        }
        bytes[cursor..cursor + name.len()].copy_from_slice(name.as_bytes());
        cursor += name.len();
        bytes[cursor] = b'=';
        cursor += 1;
        for &byte in *value {
            if unreserved(byte) {
                bytes[cursor] = byte;
                cursor += 1;
            } else {
                bytes[cursor..cursor + 3].copy_from_slice(&[
                    b'%',
                    b"0123456789ABCDEF"[(byte >> 4) as usize],
                    b"0123456789ABCDEF"[(byte & 15) as usize],
                ]);
                cursor += 3;
            }
        }
    }
    result
}
