use std::fmt;

use crate::providers::json::{self, Object, Text};
use serde::de::Visitor;
use serde::{Deserialize, Deserializer};

use super::*;
use crate::domain::WVW_BOSS_ID;

pub(super) fn decode(
    response: HttpResponse,
    current: Option<&SecretValue>,
) -> Result<DpsReportSuccess, DpsReportError> {
    let invalid = || DpsReportError {
        http_status: Some(response.status_code),
        ..DpsReportError::failed(DpsReportErrorCode::InvalidResponse)
    };
    let parsed: Response = json::decode(response.body.expose(), MAX_DPS_REPORT_RESPONSE_BYTES)
        .map_err(|_| invalid())?;
    let report = DpsReportResult {
        permalink: parsed.permalink.0,
        encounter_name: parsed.encounter.0.boss.0,
        boss_id: parsed.encounter.0.boss_id,
        success: parsed.encounter.0.success,
        mode: if parsed.encounter.0.is_legendary_cm.unwrap_or(false) {
            "LCM".into()
        } else if parsed.encounter.0.is_cm.unwrap_or(false) {
            "CM".into()
        } else if parsed.encounter.0.emboldened.unwrap_or(0) > 0 {
            format!("Emboldened {}", parsed.encounter.0.emboldened.unwrap_or(0))
        } else {
            String::new()
        },
    };
    if report.encounter_name.is_empty()
        || report.encounter_name.bytes().any(|b| b < 0x20 || b == 0x7f)
        || !valid_permalink(&report.permalink, report.boss_id)
    {
        return Err(invalid());
    }
    let mut warning = parsed
        .error
        .filter(|error| error.0)
        .map(|_| DpsReportWarning::GeneratedWithWarning);
    let replacement_user_token = match parsed.user_token {
        Some(Token::Invalid) => {
            warning = Some(DpsReportWarning::InvalidReturnedToken);
            None
        }
        Some(Token::Valid(token))
            if !token.is_empty() && current.is_none_or(|old| old.expose() != token.expose()) =>
        {
            Some(token)
        }
        _ => None,
    };
    Ok(DpsReportSuccess {
        report,
        replacement_user_token,
        warning,
    })
}
fn valid_permalink(value: &str, boss: u16) -> bool {
    crate::providers::wire::trusted_report_permalink(value)
        && (!value.starts_with("https://wvw.report/") || boss == WVW_BOSS_ID)
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Response {
    permalink: Text<2048>,
    encounter: Object<Encounter>,
    user_token: Option<Token>,
    error: Option<Warning>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Encounter {
    success: bool,
    boss_id: u16,
    boss: Text<256>,
    is_cm: Option<bool>,
    is_legendary_cm: Option<bool>,
    emboldened: Option<i32>,
}

enum Token {
    Valid(SecretValue),
    Invalid,
}
impl<'de> Deserialize<'de> for Token {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct TokenVisitor;
        impl Visitor<'_> for TokenVisitor {
            type Value = Token;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a token string")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Token, E> {
                Ok(if valid_token(value.as_bytes()) {
                    Token::Valid(SecretValue::from_bytes(value.as_bytes().to_vec()))
                } else {
                    Token::Invalid
                })
            }
        }
        deserializer.deserialize_str(TokenVisitor)
    }
}
struct Warning(bool);
impl<'de> Deserialize<'de> for Warning {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct WarningVisitor;
        impl Visitor<'_> for WarningVisitor {
            type Value = Warning;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a warning string")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Warning, E> {
                Ok(Warning(!value.is_empty()))
            }
        }
        deserializer.deserialize_str(WarningVisitor)
    }
}
