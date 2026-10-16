use super::*;
use crate::domain::{DonBotDiscordDeliveryOutcome, DonBotDiscordDeliveryReceipt};
use crate::providers::json::{self, Object, Text};
use serde::Deserialize;

pub(super) fn decode<T: serde::de::DeserializeOwned>(
    response: &HttpResponse,
) -> Result<T, DonBotError> {
    json::decode(response.body.expose(), 64 * 1024).map_err(|_| invalid(response.status_code))
}
pub(super) fn invalid(status: u16) -> DonBotError {
    DonBotError {
        http_status: Some(status),
        ..DonBotError::failed(DonBotErrorCode::InvalidResponse)
    }
}
pub(super) fn positive_id(value: u64) -> bool {
    value > 0 && value <= i64::MAX as u64
}

#[derive(Deserialize)]
pub(super) struct Delivery {
    requested: bool,
    outcome: Text<32>,
    sent: u16,
    skipped: u16,
    failed: u16,
    ambiguous: u16,
}
impl Delivery {
    pub fn receipt(self, requested: bool, maximum: u16) -> Option<DonBotDiscordDeliveryReceipt> {
        if self.requested != requested {
            return None;
        }
        let counts = [self.sent, self.skipped, self.failed, self.ambiguous];
        if counts.iter().map(|&n| u32::from(n)).sum::<u32>() > u32::from(maximum) {
            return None;
        }
        let populated = counts.iter().filter(|&&n| n != 0).count();
        use DonBotDiscordDeliveryOutcome::*;
        let outcome = match self.outcome.0.as_str() {
            "not_requested" if !requested && populated == 0 => NotRequested,
            "sent" if requested && self.sent > 0 && populated == 1 => Sent,
            "partial" if requested && populated >= 2 => Partial,
            "skipped" if requested && self.skipped > 0 && populated == 1 => Skipped,
            "failed" if requested && self.failed > 0 && populated == 1 => Failed,
            "ambiguous" if requested && self.ambiguous > 0 && populated == 1 => Ambiguous,
            _ => return None,
        };
        Some(DonBotDiscordDeliveryReceipt {
            outcome,
            sent: self.sent,
            skipped: self.skipped,
            failed: self.failed,
            ambiguous: self.ambiguous,
        })
    }
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub(super) struct Import {
    pub upload_id: u64,
    pub fight_log_id: Option<u64>,
    pub status: Text<16>,
    // Required even though duplicate imports have the same durable receipt semantics.
    #[serde(rename = "duplicate")]
    pub _duplicate: bool,
    pub discord_delivery_accepted: Option<bool>,
    pub discord_delivery: Option<Object<Delivery>>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub(super) struct Aggregate {
    pub fight_log_count: usize,
    pub discord_delivery: Object<Delivery>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Progress {
    stage: Text<256>,
    fight_log_id: Option<u64>,
    discord_delivery: Option<Object<Delivery>>,
}
#[derive(Default)]
pub(super) struct Completed {
    pub fight_log_id: Option<u64>,
    pub delivery: DonBotDiscordDeliveryReceipt,
}

pub(super) fn progress(response: &HttpResponse, requested: bool) -> Result<Completed, DonBotError> {
    if response.body.len() > 256 * 1024 {
        return Err(invalid(response.status_code));
    }
    let text =
        std::str::from_utf8(response.body.expose()).map_err(|_| invalid(response.status_code))?;
    let mut result = Completed::default();
    let mut complete = false;
    for line in text.split('\n') {
        let line = line.strip_suffix('\r').unwrap_or(line);
        let Some(data) = line.strip_prefix("data:") else {
            continue;
        };
        let data = data.strip_prefix(' ').unwrap_or(data);
        let event: Progress =
            json::decode(data.as_bytes(), 256 * 1024).map_err(|_| invalid(response.status_code))?;
        if let Some(id) = event.fight_log_id {
            if !positive_id(id) {
                return Err(invalid(response.status_code));
            }
            result.fight_log_id = Some(id);
        }
        if event.stage.0 == "failed" {
            return Err(DonBotError::failed(DonBotErrorCode::ProcessingFailed));
        }
        complete = event.stage.0 == "complete";
        if complete {
            result.delivery = delivery(event.discord_delivery, requested, response.status_code)?;
        } else if event.discord_delivery.is_some() {
            return Err(invalid(response.status_code));
        }
    }
    if !complete {
        return Err(invalid(response.status_code));
    }
    Ok(result)
}
pub(super) fn delivery(
    value: Option<Object<Delivery>>,
    requested: bool,
    status: u16,
) -> Result<DonBotDiscordDeliveryReceipt, DonBotError> {
    match value {
        Some(Object(receipt)) => receipt.receipt(requested, 4).ok_or_else(|| invalid(status)),
        None if !requested => Ok(DonBotDiscordDeliveryReceipt::default()),
        None => Err(DonBotError::failed(DonBotErrorCode::UnconfirmedDelivery)),
    }
}
