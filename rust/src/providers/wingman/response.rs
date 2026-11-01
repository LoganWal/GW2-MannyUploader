use super::{MAX_WINGMAN_RESPONSE_BYTES, WingmanError, WingmanErrorCode};
use crate::application::HttpResponse;
use crate::providers::json::{self, Object, Text};
use serde::Deserialize;

pub(super) fn decode<T: serde::de::DeserializeOwned>(
    response: &HttpResponse,
) -> Result<T, WingmanError> {
    json::decode(response.body.expose(), MAX_WINGMAN_RESPONSE_BYTES).map_err(|_| WingmanError {
        http_status: Some(response.status_code),
        ..WingmanError::failed(WingmanErrorCode::InvalidResponse)
    })
}
pub(super) fn safe_slug(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 512
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_'))
}
#[derive(Deserialize)]
pub(super) struct Accepted {
    pub result: bool,
    pub ticket: Option<u64>,
}
#[derive(Deserialize)]
pub(super) struct Ticket {
    pub state: Text<32>,
}
#[derive(Deserialize)]
pub(super) struct Log {
    pub html: Option<Text<512>>,
}
#[derive(Deserialize)]
pub(super) struct Found {
    pub success: bool,
    pub log: Option<Object<Log>>,
}
#[derive(Deserialize)]
pub(super) struct Queued {
    pub success: i32,
}
#[derive(Deserialize)]
pub(super) struct Checked {
    #[serde(rename = "inQueue")]
    pub in_queue: bool,
    #[serde(rename = "inDB")]
    pub in_db: bool,
    #[serde(rename = "targetURL")]
    pub target_url: Text<2048>,
}
