use std::time::{Duration, Instant, UNIX_EPOCH};

use manny_uploader::config::*;
use manny_uploader::domain::*;
use serde_json::{Value, json};

fn record() -> UploadJobRecord {
    UploadJobRecord {
        file: LogFileIdentity {
            canonical_path: "C:/logs/sample-\u{96ea}.zevtc".into(),
            size: 7,
            last_write_time: UNIX_EPOCH + Duration::new(1_704_067_200, 123_456_700),
        },
        detected_at: UNIX_EPOCH + Duration::from_millis(1_704_067_200_123),
        encounter_metadata: Some(EncounterMetadata {
            boss_id: 15438,
            pov_account: ":Fixture.1234".into(),
            remaining_health_basis_points: Some(4321),
        }),
        dps_report_result: Some(DpsReportResult {
            permalink: "https://dps.report/Fixture".into(),
            encounter_name: "Vale Guardian".into(),
            boss_id: 15438,
            mode: "CM".into(),
            success: true,
        }),
        wingman_upload_receipt: Some(WingmanUploadReceipt {
            permalink: "https://gw2wingman.nevermindcreations.de/log/Fixture".into(),
        }),
        donbot_upload_receipt: Some(DonBotUploadReceipt {
            upload_id: Some(101),
            fight_log_id: Some(201),
            discord_delivery: DonBotDiscordDeliveryReceipt {
                outcome: DonBotDiscordDeliveryOutcome::Sent,
                sent: 2,
                ..Default::default()
            },
            guild_id: Some("123".into()),
        }),
        twitch_delivery_receipt: Some(TwitchDeliveryReceipt {
            status: TwitchDeliveryStatus::Sent,
            message_id: Some("fixture-message".into()),
        }),
        providers: std::array::from_fn(|_| ProviderStatus {
            state: ProviderState::Succeeded,
            attempts: 1,
            detail: "Completed".into(),
        }),
    }
}

fn document() -> Value {
    serde_json::from_slice(&encode_history(&[record()], 10).unwrap()).unwrap()
}
fn decode(value: &Value) -> Result<Vec<UploadJobRecord>, HistoryDocumentError> {
    decode_history(&serde_json::to_vec(value).unwrap(), 10)
}

#[test]
fn full_receipts_match_independent_msvc_wire_fixture() {
    let fixture = include_bytes!("fixtures/history-v1.json");
    assert_eq!(decode_history(fixture, 10).unwrap(), vec![record()]);
    let bytes = encode_history(&[record()], 10).unwrap();
    assert!(bytes.ends_with(b"\n"));
    let value: Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(value, serde_json::from_slice::<Value>(fixture).unwrap());
    assert_eq!(
        value["logs"][0]["last_write_time_ticks"],
        133_485_408_001_234_567_i64
    );
}

#[test]
fn windows_timestamp_epochs_and_tick_precision_are_exact() {
    let mut value = document();
    for ticks in [
        0_i64,
        1,
        116_444_735_999_999_999,
        116_444_736_000_000_000,
        133_485_408_001_234_567,
        i64::MAX,
    ] {
        value["logs"][0]["last_write_time_ticks"] = ticks.into();
        let records = decode(&value).unwrap();
        let encoded: Value =
            serde_json::from_slice(&encode_history(&records, 10).unwrap()).unwrap();
        assert_eq!(encoded["logs"][0]["last_write_time_ticks"], ticks);
    }
    value["logs"][0]["last_write_time_ticks"] = (-1).into();
    assert_eq!(decode(&value), Err(HistoryDocumentError::InvalidRecord));
    // Windows SystemTime itself has 100 ns precision. Unix can represent finer instants.
    #[cfg(unix)]
    {
        let mut input = record();
        input.file.last_write_time = UNIX_EPOCH - Duration::from_secs(11_644_473_601);
        assert_eq!(
            encode_history(&[input], 10),
            Err(HistoryDocumentError::InvalidRecord)
        );
        let mut input = record();
        input.file.last_write_time += Duration::from_nanos(1);
        assert_eq!(
            encode_history(&[input], 10),
            Err(HistoryDocumentError::InvalidRecord)
        );
    }
}

#[test]
fn detection_time_matches_cpp_truncation_on_both_sides_of_unix_epoch() {
    for (time, milliseconds) in [
        (UNIX_EPOCH, 0_i64),
        (UNIX_EPOCH + Duration::from_micros(1999), 1),
        (UNIX_EPOCH - Duration::from_micros(1999), -1),
    ] {
        let mut input = record();
        input.detected_at = time;
        let encoded: Value =
            serde_json::from_slice(&encode_history(&[input], 10).unwrap()).unwrap();
        assert_eq!(encoded["logs"][0]["detected_at_milliseconds"], milliseconds);
        let restored = decode(&encoded).unwrap();
        assert_eq!(
            restored[0].detected_at,
            if milliseconds < 0 {
                UNIX_EPOCH - Duration::from_millis(milliseconds.unsigned_abs())
            } else {
                UNIX_EPOCH + Duration::from_millis(milliseconds as u64)
            }
        );
    }
}

#[test]
fn interrupted_states_normalize_without_persisting_deadlines() {
    let states = [
        ProviderState::Disabled,
        ProviderState::Waiting,
        ProviderState::Active,
        ProviderState::Succeeded,
        ProviderState::Skipped,
        ProviderState::RetryScheduled(Instant::now()),
        ProviderState::Failed,
        ProviderState::Cancelled,
    ];
    for (code, state) in states.into_iter().enumerate() {
        let mut input = record();
        input.providers[0].state = state;
        input.providers[0].attempts = u32::MAX;
        let bytes = encode_history(&[input.clone()], 10).unwrap();
        let wire: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(wire["logs"][0]["providers"][0]["state"], code);
        assert!(
            !String::from_utf8(bytes.clone())
                .unwrap()
                .contains("retry_at")
        );
        if state.is_busy() {
            input.providers[0].state = ProviderState::Failed;
            input.providers[0].detail = "Interrupted by the previous game session".into();
        }
        assert_eq!(decode_history(&bytes, 10).unwrap(), vec![input]);
    }
}

#[test]
fn optional_receipts_and_legacy_missing_guild_remain_valid() {
    let mut value = document();
    value["logs"][0]["donbot_upload_receipt"]
        .as_object_mut()
        .unwrap()
        .remove("guild_id");
    let restored = decode(&value).unwrap();
    assert!(
        restored[0]
            .donbot_upload_receipt
            .as_ref()
            .unwrap()
            .guild_id
            .is_none()
    );
    for key in [
        "encounter_metadata",
        "dps_report_result",
        "wingman_upload_receipt",
        "donbot_upload_receipt",
        "twitch_delivery_receipt",
    ] {
        value["logs"][0].as_object_mut().unwrap().remove(key);
    }
    let restored = decode(&value).unwrap();
    assert!(restored[0].encounter_metadata.is_none());
    assert!(restored[0].twitch_delivery_receipt.is_none());
    assert_eq!(
        decode_history(&encode_history(&restored, 10).unwrap(), 10).unwrap(),
        restored
    );
}

#[test]
fn malformed_schema_shapes_keys_and_scalars_are_rejected() {
    for bytes in [
        b"".as_slice(),
        b"{}",
        b"[]",
        b"{\"schema_version\":1,\"logs\":[] ,\"schema_version\":1}",
        b"{\"schema_version\":1,\"logs\":[]} true",
        b"{\"schema_version\":1,\"logs\":[],\"token\":\"synthetic\"}",
        b"{\"schema_version\":1,\"logs\":[]}\xff",
    ] {
        assert!(decode_history(bytes, 10).is_err());
    }
    for (pointer, replacement) in [
        ("/schema_version", json!(2)),
        ("/schema_version", json!(1.0)),
        ("/logs", json!({})),
        ("/logs/0", json!([])),
        ("/logs/0/size", json!(0)),
        ("/logs/0/size", json!(-1)),
        ("/logs/0/path", json!("")),
        ("/logs/0/path", json!("synthetic\npath")),
        ("/logs/0/encounter_metadata", json!([15438, "test", null])),
        ("/logs/0/providers", json!([])),
        ("/logs/0/providers/0", json!([3, 1, "Completed"])),
        ("/logs/0/providers/0/state", json!(8)),
        ("/logs/0/providers/0/attempts", json!(4_294_967_296_u64)),
        (
            "/logs/0/encounter_metadata/remaining_health_basis_points",
            json!(10001),
        ),
    ] {
        let mut value = document();
        *value.pointer_mut(pointer).unwrap() = replacement;
        assert!(decode(&value).is_err(), "accepted {pointer}");
    }
    for pointer in [
        "",
        "/logs/0",
        "/logs/0/providers/0",
        "/logs/0/encounter_metadata",
        "/logs/0/dps_report_result",
        "/logs/0/wingman_upload_receipt",
        "/logs/0/donbot_upload_receipt",
        "/logs/0/twitch_delivery_receipt",
    ] {
        let mut value = document();
        value
            .pointer_mut(pointer)
            .unwrap()
            .as_object_mut()
            .unwrap()
            .insert("access_token".into(), json!("synthetic-only"));
        assert_eq!(decode(&value), Err(HistoryDocumentError::InvalidJson));
    }
    let original = String::from_utf8(encode_history(&[record()], 10).unwrap()).unwrap();
    for bad in [
        original.replace("\"size\": 7", "\"size\": 7, \"size\": 7"),
        original.replace("Completed", "\\ud800"),
    ] {
        assert_eq!(
            decode_history(bad.as_bytes(), 10),
            Err(HistoryDocumentError::InvalidJson)
        );
    }
}

#[test]
fn text_bounds_are_utf8_bytes_and_checked_on_read_and_write() {
    for (pointer, limit) in [
        ("/logs/0/path", 4096),
        ("/logs/0/encounter_metadata/pov_account", 256),
        ("/logs/0/dps_report_result/encounter_name", 256),
        ("/logs/0/dps_report_result/mode", 64),
        ("/logs/0/providers/0/detail", 4096),
    ] {
        let mut value = document();
        *value.pointer_mut(pointer).unwrap() = "x".repeat(limit).into();
        assert!(decode(&value).is_ok());
        *value.pointer_mut(pointer).unwrap() = format!("{}\u{96ea}", "x".repeat(limit - 2)).into();
        assert!(decode(&value).is_err());
    }
    let mut input = record();
    input.providers[0].detail = "x".repeat(4097);
    assert_eq!(
        encode_history(&[input], 10),
        Err(HistoryDocumentError::InvalidRecord)
    );
}

#[test]
fn untrusted_links_and_invalid_normalized_receipts_are_rejected() {
    for (pointer, replacement) in [
        (
            "/logs/0/dps_report_result/permalink",
            json!("https://dps.report.evil.test/x"),
        ),
        (
            "/logs/0/dps_report_result/permalink",
            json!("https://dps.report/"),
        ),
        (
            "/logs/0/dps_report_result/permalink",
            json!("https://dps.report/test\\bad"),
        ),
        (
            "/logs/0/dps_report_result/permalink",
            json!("https://wvw.report/test"),
        ),
        (
            "/logs/0/wingman_upload_receipt/permalink",
            json!("https://evil.test/x"),
        ),
        ("/logs/0/donbot_upload_receipt/upload_id", json!(0)),
        (
            "/logs/0/donbot_upload_receipt/fight_log_id",
            json!(i64::MAX as u64 + 1),
        ),
        ("/logs/0/donbot_upload_receipt/guild_id", json!("0123")),
        ("/logs/0/donbot_upload_receipt/discord_sent", json!(5)),
        (
            "/logs/0/donbot_upload_receipt/discord_delivery_outcome",
            json!(6),
        ),
        (
            "/logs/0/donbot_upload_receipt/discord_delivery_outcome",
            json!(0),
        ),
        ("/logs/0/twitch_delivery_receipt/status", json!(10)),
        ("/logs/0/twitch_delivery_receipt/status", json!(1)),
        ("/logs/0/twitch_delivery_receipt/message_id", json!(null)),
        (
            "/logs/0/twitch_delivery_receipt/message_id",
            json!("has space"),
        ),
    ] {
        let mut value = document();
        *value.pointer_mut(pointer).unwrap() = replacement;
        assert!(decode(&value).is_err(), "accepted {pointer}");
    }
    for origin in [
        "https://dps.report/",
        "https://b.dps.report/",
        "https://wvw.report/",
    ] {
        let mut input = record();
        let report = input.dps_report_result.as_mut().unwrap();
        report.boss_id = WVW_BOSS_ID;
        report.permalink = format!("{origin}valid");
        assert!(encode_history(&[input], 10).is_ok());
    }
}

#[test]
fn all_normalized_delivery_codes_preserve_semantics() {
    let outcomes = [
        DonBotDiscordDeliveryOutcome::NotRequested,
        DonBotDiscordDeliveryOutcome::Sent,
        DonBotDiscordDeliveryOutcome::Partial,
        DonBotDiscordDeliveryOutcome::Skipped,
        DonBotDiscordDeliveryOutcome::Failed,
        DonBotDiscordDeliveryOutcome::Ambiguous,
    ];
    for (code, outcome) in outcomes.into_iter().enumerate() {
        let mut input = record();
        let receipt = &mut input
            .donbot_upload_receipt
            .as_mut()
            .unwrap()
            .discord_delivery;
        *receipt = DonBotDiscordDeliveryReceipt {
            outcome,
            sent: u16::from(matches!(code, 1 | 2)),
            skipped: u16::from(matches!(code, 2 | 3)),
            failed: u16::from(code == 4),
            ambiguous: u16::from(code == 5),
        };
        let bytes = encode_history(&[input.clone()], 10).unwrap();
        assert_eq!(decode_history(&bytes, 10).unwrap(), vec![input]);
        let value: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(
            value["logs"][0]["donbot_upload_receipt"]["discord_delivery_outcome"],
            code
        );
    }
    for code in 1..=9 {
        let mut value = document();
        value["logs"][0]["twitch_delivery_receipt"]["status"] = code.into();
        value["logs"][0]["twitch_delivery_receipt"]["message_id"] = Value::Null;
        let decoded = decode(&value).unwrap();
        let encoded: Value =
            serde_json::from_slice(&encode_history(&decoded, 10).unwrap()).unwrap();
        assert_eq!(encoded, value);
    }
}

#[test]
fn duplicate_identity_and_record_capacity_are_enforced() {
    for capacity in [0, MAX_HISTORY_RECORDS + 1] {
        assert_eq!(
            encode_history(&[], capacity),
            Err(HistoryDocumentError::InvalidCapacity)
        );
        assert_eq!(
            decode_history(b"{}", capacity),
            Err(HistoryDocumentError::InvalidCapacity)
        );
    }
    assert_eq!(
        encode_history(&[record(), record()], 10),
        Err(HistoryDocumentError::DuplicateIdentity)
    );
    let mut value = document();
    let duplicate = value["logs"][0].clone();
    value["logs"].as_array_mut().unwrap().push(duplicate);
    assert_eq!(decode(&value), Err(HistoryDocumentError::DuplicateIdentity));
    assert_eq!(
        decode_history(&serde_json::to_vec(&value).unwrap(), 1),
        Err(HistoryDocumentError::TooLarge)
    );
    value["logs"][1]["size"] = 8.into();
    assert!(decode(&value).is_ok());
    value["logs"][1]["size"] = 7.into();
    value["logs"][1]["last_write_time_ticks"] = 133_485_408_001_234_568_i64.into();
    assert!(decode(&value).is_ok());
    let empty = json!({"schema_version": 1, "logs": []});
    assert_eq!(decode(&empty).unwrap(), vec![]);
}

#[test]
fn document_bounds_apply_before_parse_and_during_serialization() {
    let mut bytes = br#"{"schema_version":1,"logs":[]}"#.to_vec();
    bytes.resize(MAX_HISTORY_BYTES, b' ');
    assert!(decode_history(&bytes, 10).is_ok());
    bytes.push(b' ');
    assert_eq!(
        decode_history(&bytes, 10),
        Err(HistoryDocumentError::TooLarge)
    );
    let mut input = record();
    for status in &mut input.providers {
        status.detail = "x".repeat(4096);
    }
    let records: Vec<_> = (1..=2100)
        .map(|size| {
            let mut item = input.clone();
            item.file.size = size;
            item
        })
        .collect();
    assert_eq!(
        encode_history(&records, MAX_HISTORY_RECORDS),
        Err(HistoryDocumentError::TooLarge)
    );
    let oversized_count = format!(
        "{{\"schema_version\":1,\"logs\":[{}]}}",
        vec![serde_json::to_string(&document()["logs"][0]).unwrap(); MAX_HISTORY_RECORDS + 1]
            .join(",")
    );
    assert_eq!(
        decode_history(oversized_count.as_bytes(), MAX_HISTORY_RECORDS),
        Err(HistoryDocumentError::InvalidJson)
    );
}

#[cfg(windows)]
#[test]
fn extended_windows_paths_write_cpp_compatible_drive_and_unc_identities() {
    for (extended, ordinary) in [
        (r"\\?\C:\logs\sample.zevtc", r"C:\logs\sample.zevtc"),
        (
            r"\\?\UNC\server\share\logs\sample.zevtc",
            r"\\server\share\logs\sample.zevtc",
        ),
    ] {
        let mut input = record();
        input.file.canonical_path = extended.into();
        let bytes = encode_history(&[input.clone()], 10).unwrap();
        let value: Value = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(value["logs"][0]["path"], ordinary);
        assert_eq!(decode_history(&bytes, 10).unwrap(), vec![input]);
    }
}

#[cfg(unix)]
#[test]
fn non_utf8_native_path_is_rejected_without_lossy_conversion() {
    use std::os::unix::ffi::OsStringExt;
    let mut input = record();
    input.file.canonical_path = std::ffi::OsString::from_vec(vec![b'/', 0xff]).into();
    assert_eq!(
        encode_history(&[input], 10),
        Err(HistoryDocumentError::InvalidRecord)
    );
}
