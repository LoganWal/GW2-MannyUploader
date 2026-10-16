use std::collections::HashSet;
use std::fmt;
use std::io::Write;
use std::marker::PhantomData;

use serde::de::{DeserializeSeed, MapAccess, SeqAccess, Visitor};
use serde::ser::SerializeSeq;
use serde::{Deserialize, Deserializer, Serialize, Serializer};

use super::history_wire::*;
use crate::domain::*;

pub const MAX_HISTORY_BYTES: usize = 32 * 1024 * 1024;
pub const MAX_HISTORY_RECORDS: usize = 10_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HistoryDocumentError {
    InvalidCapacity,
    TooLarge,
    InvalidJson,
    InvalidSchema,
    InvalidRecord,
    DuplicateIdentity,
    SerializationFailed,
}
type Error = HistoryDocumentError;

pub fn decode_history(document: &[u8], capacity: usize) -> Result<Vec<UploadJobRecord>, Error> {
    validate_capacity(capacity)?;
    if document.len() > MAX_HISTORY_BYTES {
        return Err(Error::TooLarge);
    }
    let Object(wire): Object<History> =
        serde_json::from_slice(document).map_err(|_| Error::InvalidJson)?;
    if wire.schema_version != 1 {
        return Err(Error::InvalidSchema);
    }
    if wire.logs.len() > capacity {
        return Err(Error::TooLarge);
    }
    let records: Vec<_> = wire
        .logs
        .into_iter()
        .map(|Object(log)| log.decode())
        .collect::<Result<_, _>>()?;
    validate_records(&records, capacity)?;
    Ok(records)
}

pub fn encode_history(records: &[UploadJobRecord], capacity: usize) -> Result<Vec<u8>, Error> {
    validate_records(records, capacity)?;
    let mut output = BoundedOutput {
        bytes: Vec::new(),
        too_large: false,
    };
    #[derive(Serialize)]
    struct Document<'a> {
        schema_version: u32,
        logs: Records<'a>,
    }
    let result = Document {
        schema_version: 1,
        logs: Records(records),
    }
    .serialize(&mut serde_json::Serializer::pretty(&mut output));
    if output.too_large {
        return Err(Error::TooLarge);
    }
    result.map_err(|_| Error::SerializationFailed)?;
    output.write_all(b"\n").map_err(|_| {
        if output.too_large {
            Error::TooLarge
        } else {
            Error::SerializationFailed
        }
    })?;
    Ok(output.bytes)
}

pub(super) fn validate_capacity(capacity: usize) -> Result<(), Error> {
    if (1..=MAX_HISTORY_RECORDS).contains(&capacity) {
        Ok(())
    } else {
        Err(Error::InvalidCapacity)
    }
}

fn validate_records(records: &[UploadJobRecord], capacity: usize) -> Result<(), Error> {
    validate_capacity(capacity)?;
    if records.len() > capacity {
        return Err(Error::TooLarge);
    }
    let mut identities = HashSet::with_capacity(records.len());
    for record in records {
        validate_record(record)?;
        if !identities.insert(&record.file) {
            return Err(Error::DuplicateIdentity);
        }
    }
    Ok(())
}

fn validate_record(record: &UploadJobRecord) -> Result<(), Error> {
    let path = encode_unc_path(&record.file.canonical_path)?;
    let path = match &path {
        Some(path) => path.as_str(),
        None => encode_path(&record.file.canonical_path)?,
    };
    if path.is_empty() || record.file.size == 0 {
        return Err(Error::InvalidRecord);
    }
    text(path, 4096)?;
    encode_file_time(record.file.last_write_time)?;
    encode_detected_time(record.detected_at)?;
    if let Some(metadata) = &record.encounter_metadata {
        text(&metadata.pov_account, 256)?;
        if metadata
            .remaining_health_basis_points
            .is_some_and(|health| health > 10_000)
        {
            return Err(Error::InvalidRecord);
        }
    }
    if let Some(result) = &record.dps_report_result {
        text(&result.encounter_name, 256)?;
        text(&result.mode, 64)?;
        let origins = if result.boss_id == WVW_BOSS_ID {
            &[
                "https://dps.report/",
                "https://b.dps.report/",
                "https://wvw.report/",
            ][..]
        } else {
            &["https://dps.report/", "https://b.dps.report/"][..]
        };
        report_url(&result.permalink, origins)?;
    }
    if let Some(receipt) = &record.wingman_upload_receipt {
        report_url(
            &receipt.permalink,
            &["https://gw2wingman.nevermindcreations.de/log/"],
        )?;
    }
    for status in &record.providers {
        text(&status.detail, 4096)?;
    }
    if let Some(receipt) = &record.donbot_upload_receipt {
        if let Some(guild) = &receipt.guild_id {
            text(guild, 19)?;
        }
    }
    if let Some(receipt) = &record.twitch_delivery_receipt {
        if let Some(message) = &receipt.message_id {
            text(message, 256)?;
        }
    }
    // Share the domain's normalized receipt rules without changing the saved states.
    UploadJob::restore(
        UploadJobId::new(1).expect("nonzero constant"),
        record.clone(),
    )
    .map_err(|_| Error::InvalidRecord)?;
    Ok(())
}

fn text(value: &str, limit: usize) -> Result<(), Error> {
    if value.len() > limit || value.bytes().any(|byte| byte < 0x20 && byte != b'\t') {
        Err(Error::InvalidRecord)
    } else {
        Ok(())
    }
}

fn report_url(value: &str, origins: &[&str]) -> Result<(), Error> {
    if value.len() > 2048
        || !value
            .bytes()
            .all(|b| (0x21..=0x7e).contains(&b) && !matches!(b, b'\\' | b'"'))
        || !origins.iter().any(|origin| {
            value
                .strip_prefix(origin)
                .is_some_and(|suffix| !suffix.is_empty())
        })
    {
        Err(Error::InvalidRecord)
    } else {
        Ok(())
    }
}

struct BoundedOutput {
    bytes: Vec<u8>,
    too_large: bool,
}
impl Write for BoundedOutput {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        if bytes.len() > MAX_HISTORY_BYTES - self.bytes.len() {
            self.too_large = true;
            return Err(std::io::Error::other("history size limit"));
        }
        self.bytes
            .try_reserve(bytes.len())
            .map_err(|_| std::io::Error::other("history allocation failed"))?;
        self.bytes.extend_from_slice(bytes);
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

struct Records<'a>(&'a [UploadJobRecord]);
impl Serialize for Records<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut sequence = serializer.serialize_seq(Some(self.0.len()))?;
        for record in self.0 {
            let wire = Record::encode(record)
                .map_err(|_| serde::ser::Error::custom("invalid history record"))?;
            sequence.serialize_element(&wire)?;
        }
        sequence.end()
    }
}

// Force named objects, since derived Serde structs otherwise also accept positional arrays.
#[derive(Serialize)]
#[serde(transparent)]
struct Object<T>(T);
impl<'de, T: Deserialize<'de>> Deserialize<'de> for Object<T> {
    fn deserialize<D: Deserializer<'de>>(decoder: D) -> Result<Self, D::Error> {
        struct ObjectVisitor<T>(PhantomData<T>);
        impl<'de, T: Deserialize<'de>> Visitor<'de> for ObjectVisitor<T> {
            type Value = Object<T>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a history object")
            }
            fn visit_map<M: MapAccess<'de>>(self, map: M) -> Result<Self::Value, M::Error> {
                T::deserialize(serde::de::value::MapAccessDeserializer::new(map)).map(Object)
            }
        }
        decoder.deserialize_map(ObjectVisitor(PhantomData))
    }
}

#[derive(Serialize)]
#[serde(transparent)]
struct Text<const N: usize>(String);
impl<'de, const N: usize> Deserialize<'de> for Text<N> {
    fn deserialize<D: Deserializer<'de>>(decoder: D) -> Result<Self, D::Error> {
        struct TextVisitor<const N: usize>;
        impl<const N: usize> Visitor<'_> for TextVisitor<N> {
            type Value = Text<N>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("bounded UTF-8 text")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Self::Value, E> {
                text(value, N).map_err(|_| E::custom("invalid history text"))?;
                Ok(Text(value.into()))
            }
        }
        decoder.deserialize_str(TextVisitor::<N>)
    }
}

fn logs<'de, D: Deserializer<'de>>(decoder: D) -> Result<Vec<Object<Record>>, D::Error> {
    struct Logs;
    impl<'de> Visitor<'de> for Logs {
        type Value = Vec<Object<Record>>;
        fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str("bounded history records")
        }
        fn visit_seq<A: SeqAccess<'de>>(self, mut sequence: A) -> Result<Self::Value, A::Error> {
            let mut records = Vec::new();
            while records.len() < MAX_HISTORY_RECORDS {
                match sequence.next_element()? {
                    Some(record) => records.push(record),
                    None => return Ok(records),
                }
            }
            struct Reject;
            impl<'de> DeserializeSeed<'de> for Reject {
                type Value = ();
                fn deserialize<D: Deserializer<'de>>(self, _: D) -> Result<(), D::Error> {
                    Err(serde::de::Error::custom("history record limit"))
                }
            }
            sequence.next_element_seed(Reject)?;
            Ok(records)
        }
    }
    decoder.deserialize_seq(Logs)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct History {
    schema_version: u32,
    #[serde(deserialize_with = "logs")]
    logs: Vec<Object<Record>>,
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
    path: Text<4096>,
    size: u64,
    last_write_time_ticks: i64,
    detected_at_milliseconds: i64,
    encounter_metadata: Option<Object<Metadata>>,
    dps_report_result: Option<Object<Report>>,
    wingman_upload_receipt: Option<Object<Wingman>>,
    donbot_upload_receipt: Option<Object<DonBot>>,
    twitch_delivery_receipt: Option<Object<Twitch>>,
    providers: [Object<Status>; 4],
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Metadata {
    boss_id: u16,
    pov_account: Text<256>,
    remaining_health_basis_points: Option<u16>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Report {
    permalink: Text<2048>,
    encounter_name: Text<256>,
    boss_id: u16,
    mode: Text<64>,
    success: bool,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Wingman {
    permalink: Text<2048>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct DonBot {
    upload_id: Option<u64>,
    fight_log_id: Option<u64>,
    discord_delivery_outcome: u8,
    discord_sent: u16,
    discord_skipped: u16,
    discord_failed: u16,
    discord_ambiguous: u16,
    guild_id: Option<Text<19>>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Twitch {
    status: u8,
    message_id: Option<Text<256>>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Status {
    state: u8,
    attempts: u32,
    detail: Text<4096>,
}

impl Record {
    fn decode(self) -> Result<UploadJobRecord, Error> {
        let mut providers = std::array::from_fn(|_| ProviderStatus::default());
        for (target, Object(source)) in providers.iter_mut().zip(self.providers) {
            *target = ProviderStatus {
                state: match source.state {
                    0 => ProviderState::Disabled,
                    // No retry deadline is persisted. Route all interrupted states through domain restore.
                    1 | 2 | 5 => ProviderState::Waiting,
                    3 => ProviderState::Succeeded,
                    4 => ProviderState::Skipped,
                    6 => ProviderState::Failed,
                    7 => ProviderState::Cancelled,
                    _ => return Err(Error::InvalidRecord),
                },
                attempts: source.attempts,
                detail: source.detail.0,
            };
        }
        let record = UploadJobRecord {
            file: LogFileIdentity {
                canonical_path: self.path.0.into(),
                size: self.size,
                last_write_time: decode_file_time(self.last_write_time_ticks)?,
            },
            detected_at: decode_detected_time(self.detected_at_milliseconds)?,
            encounter_metadata: self.encounter_metadata.map(|Object(m)| EncounterMetadata {
                boss_id: m.boss_id,
                pov_account: m.pov_account.0,
                remaining_health_basis_points: m.remaining_health_basis_points,
            }),
            dps_report_result: self.dps_report_result.map(|Object(r)| DpsReportResult {
                permalink: r.permalink.0,
                encounter_name: r.encounter_name.0,
                boss_id: r.boss_id,
                mode: r.mode.0,
                success: r.success,
            }),
            wingman_upload_receipt: self.wingman_upload_receipt.map(|Object(r)| {
                WingmanUploadReceipt {
                    permalink: r.permalink.0,
                }
            }),
            donbot_upload_receipt: self
                .donbot_upload_receipt
                .map(|Object(r)| r.decode())
                .transpose()?,
            twitch_delivery_receipt: self
                .twitch_delivery_receipt
                .map(|Object(r)| r.decode())
                .transpose()?,
            providers,
        };
        validate_record(&record)?;
        UploadJob::restore(UploadJobId::new(1).expect("nonzero constant"), record)
            .map(|job| job.snapshot().clone())
            .map_err(|_| Error::InvalidRecord)
    }

    fn encode(r: &UploadJobRecord) -> Result<Self, Error> {
        Ok(Self {
            path: Text(match encode_unc_path(&r.file.canonical_path)? {
                Some(path) => path,
                None => encode_path(&r.file.canonical_path)?.into(),
            }),
            size: r.file.size,
            last_write_time_ticks: encode_file_time(r.file.last_write_time)?,
            detected_at_milliseconds: encode_detected_time(r.detected_at)?,
            encounter_metadata: r.encounter_metadata.as_ref().map(|m| {
                Object(Metadata {
                    boss_id: m.boss_id,
                    pov_account: Text(m.pov_account.clone()),
                    remaining_health_basis_points: m.remaining_health_basis_points,
                })
            }),
            dps_report_result: r.dps_report_result.as_ref().map(|r| {
                Object(Report {
                    permalink: Text(r.permalink.clone()),
                    encounter_name: Text(r.encounter_name.clone()),
                    boss_id: r.boss_id,
                    mode: Text(r.mode.clone()),
                    success: r.success,
                })
            }),
            wingman_upload_receipt: r.wingman_upload_receipt.as_ref().map(|r| {
                Object(Wingman {
                    permalink: Text(r.permalink.clone()),
                })
            }),
            donbot_upload_receipt: r.donbot_upload_receipt.as_ref().map(|r| {
                Object(DonBot {
                    upload_id: r.upload_id,
                    fight_log_id: r.fight_log_id,
                    discord_delivery_outcome: discord_code(r.discord_delivery.outcome),
                    discord_sent: r.discord_delivery.sent,
                    discord_skipped: r.discord_delivery.skipped,
                    discord_failed: r.discord_delivery.failed,
                    discord_ambiguous: r.discord_delivery.ambiguous,
                    guild_id: r.guild_id.clone().map(Text),
                })
            }),
            twitch_delivery_receipt: r.twitch_delivery_receipt.as_ref().map(|r| {
                Object(Twitch {
                    status: twitch_code(r.status),
                    message_id: r.message_id.clone().map(Text),
                })
            }),
            providers: r.providers.each_ref().map(|s| {
                Object(Status {
                    state: state_code(s.state),
                    attempts: s.attempts,
                    detail: Text(s.detail.clone()),
                })
            }),
        })
    }
}

const DISCORD: [DonBotDiscordDeliveryOutcome; 6] = [
    DonBotDiscordDeliveryOutcome::NotRequested,
    DonBotDiscordDeliveryOutcome::Sent,
    DonBotDiscordDeliveryOutcome::Partial,
    DonBotDiscordDeliveryOutcome::Skipped,
    DonBotDiscordDeliveryOutcome::Failed,
    DonBotDiscordDeliveryOutcome::Ambiguous,
];
const TWITCH: [TwitchDeliveryStatus; 10] = [
    TwitchDeliveryStatus::Sent,
    TwitchDeliveryStatus::AutoMod,
    TwitchDeliveryStatus::BlockedTerm,
    TwitchDeliveryStatus::Duplicate,
    TwitchDeliveryStatus::RateLimited,
    TwitchDeliveryStatus::FollowersOnly,
    TwitchDeliveryStatus::SlowMode,
    TwitchDeliveryStatus::SubscribersOnly,
    TwitchDeliveryStatus::Restricted,
    TwitchDeliveryStatus::OtherDrop,
];
fn discord_code(value: DonBotDiscordDeliveryOutcome) -> u8 {
    DISCORD
        .iter()
        .position(|&v| v == value)
        .expect("all outcomes") as u8
}
fn twitch_code(value: TwitchDeliveryStatus) -> u8 {
    TWITCH
        .iter()
        .position(|&v| v == value)
        .expect("all outcomes") as u8
}
fn state_code(value: ProviderState) -> u8 {
    match value {
        ProviderState::Disabled => 0,
        ProviderState::Waiting => 1,
        ProviderState::Active => 2,
        ProviderState::Succeeded => 3,
        ProviderState::Skipped => 4,
        ProviderState::RetryScheduled(_) => 5,
        ProviderState::Failed => 6,
        ProviderState::Cancelled => 7,
    }
}
impl DonBot {
    fn decode(self) -> Result<DonBotUploadReceipt, Error> {
        Ok(DonBotUploadReceipt {
            upload_id: self.upload_id,
            fight_log_id: self.fight_log_id,
            guild_id: self.guild_id.map(|s| s.0),
            discord_delivery: DonBotDiscordDeliveryReceipt {
                outcome: *DISCORD
                    .get(usize::from(self.discord_delivery_outcome))
                    .ok_or(Error::InvalidRecord)?,
                sent: self.discord_sent,
                skipped: self.discord_skipped,
                failed: self.discord_failed,
                ambiguous: self.discord_ambiguous,
            },
        })
    }
}
impl Twitch {
    fn decode(self) -> Result<TwitchDeliveryReceipt, Error> {
        Ok(TwitchDeliveryReceipt {
            status: *TWITCH
                .get(usize::from(self.status))
                .ok_or(Error::InvalidRecord)?,
            message_id: self.message_id.map(|s| s.0),
        })
    }
}
