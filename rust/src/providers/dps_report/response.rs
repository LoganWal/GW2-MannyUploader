use std::fmt;
use std::marker::PhantomData;

use serde::de::{MapAccess, Visitor, value::MapAccessDeserializer};
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
    if response.body.len() > MAX_DPS_REPORT_RESPONSE_BYTES {
        return Err(invalid());
    }
    let text = std::str::from_utf8(response.body.expose()).map_err(|_| invalid())?;
    // IgnoredAny skips Unicode validation in unknown strings. Validate those without retaining values.
    if !valid_unicode_escapes(text.as_bytes()) {
        return Err(invalid());
    }
    let Object(parsed): Object<Response> = serde_json::from_str(text).map_err(|_| invalid())?;
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
    value.len() <= 2048
        && value
            .bytes()
            .all(|b| (0x21..=0x7e).contains(&b) && !matches!(b, b'@' | b'?' | b'#' | b'\\' | b'"'))
        && [
            "https://dps.report/",
            "https://b.dps.report/",
            "https://wvw.report/",
        ]
        .iter()
        .enumerate()
        .any(|(index, prefix)| {
            (index != 2 || boss == WVW_BOSS_ID)
                && value
                    .strip_prefix(prefix)
                    .is_some_and(|path| !path.is_empty())
        })
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

struct Object<T>(T);
impl<'de, T: Deserialize<'de>> Deserialize<'de> for Object<T> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct ObjectVisitor<T>(PhantomData<T>);
        impl<'de, T: Deserialize<'de>> Visitor<'de> for ObjectVisitor<T> {
            type Value = Object<T>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("an object")
            }
            fn visit_map<A: MapAccess<'de>>(self, map: A) -> Result<Self::Value, A::Error> {
                T::deserialize(MapAccessDeserializer::new(map)).map(Object)
            }
        }
        deserializer.deserialize_map(ObjectVisitor(PhantomData))
    }
}
struct Text<const LIMIT: usize>(String);
impl<'de, const LIMIT: usize> Deserialize<'de> for Text<LIMIT> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct TextVisitor<const N: usize>;
        impl<const N: usize> Visitor<'_> for TextVisitor<N> {
            type Value = Text<N>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("bounded text")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Self::Value, E> {
                if value.len() > N {
                    return Err(E::custom("text limit"));
                }
                Ok(Text(value.into()))
            }
        }
        deserializer.deserialize_str(TextVisitor::<LIMIT>)
    }
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

fn valid_unicode_escapes(bytes: &[u8]) -> bool {
    let mut quoted = false;
    let mut index = 0;
    while index < bytes.len() {
        match bytes[index] {
            b'"' => {
                quoted = !quoted;
                index += 1;
            }
            b'\\' if quoted => {
                index += 1;
                if bytes.get(index) == Some(&b'u') {
                    let Some(unit) = unicode_unit(bytes, index + 1) else {
                        return false;
                    };
                    index += 5;
                    if (0xd800..=0xdbff).contains(&unit) {
                        if bytes.get(index..index + 2) != Some(b"\\u") {
                            return false;
                        }
                        let Some(low) = unicode_unit(bytes, index + 2) else {
                            return false;
                        };
                        if !(0xdc00..=0xdfff).contains(&low) {
                            return false;
                        }
                        index += 6;
                    } else if (0xdc00..=0xdfff).contains(&unit) {
                        return false;
                    }
                } else {
                    index += 1;
                }
            }
            _ => index += 1,
        }
    }
    true
}
fn unicode_unit(bytes: &[u8], index: usize) -> Option<u16> {
    bytes
        .get(index..index + 4)?
        .iter()
        .try_fold(0u16, |value, byte| {
            let digit = match byte {
                b'0'..=b'9' => byte - b'0',
                b'a'..=b'f' => byte - b'a' + 10,
                b'A'..=b'F' => byte - b'A' + 10,
                _ => return None,
            };
            Some(value * 16 + u16::from(digit))
        })
}
