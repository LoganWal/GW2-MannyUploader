use manny_uploader::application::*;
use std::time::{Duration, SystemTime};

fn session() -> TwitchSession {
    TwitchSession {
        credentials: TwitchCredentialSet {
            access_token: SecretValue::from_bytes(b"ACCESS".to_vec()),
            refresh_token: SecretValue::from_bytes(b"REFRESH".to_vec()),
            access_expires_at: SystemTime::UNIX_EPOCH + Duration::from_secs(1_800_000_000),
        },
        user_id: "123".into(),
        login: "test_user".into(),
        scopes: vec![TWITCH_CHAT_SCOPE.into()],
    }
}
fn fixture() -> SecretValue {
    let mut bytes = b"MNYTWSN1\x01\x00\x00\x00\x00\xd2\x49\x6b\x00\x00\x00\x00\x06\x00\x00\x00\x07\x00\x00\x00\x03\x00\x00\x00\x09\x00\x00\x00\x01\x00\x00\x00".to_vec();
    bytes.extend_from_slice(b"ACCESSREFRESH123test_user\x0f\x00\x00\x00user:write:chat");
    SecretValue::from_bytes(bytes)
}

#[test]
fn exact_v1_wire_fixture_round_trips_all_fields() {
    let session = session();
    let encoded = encode_twitch_session(&session).unwrap();
    let fixture = fixture();
    assert_eq!(encoded.expose(), fixture.expose());
    let decoded = decode_twitch_session(&fixture).unwrap();
    assert_eq!(decoded.credentials.access_token.expose(), b"ACCESS");
    assert_eq!(decoded.credentials.refresh_token.expose(), b"REFRESH");
    assert_eq!(
        decoded.credentials.access_expires_at,
        session.credentials.access_expires_at
    );
    assert_eq!(decoded.user_id, session.user_id);
    assert_eq!(decoded.login, session.login);
    assert_eq!(decoded.scopes, session.scopes);
    assert_eq!(
        encode_twitch_session(&decoded).unwrap().expose(),
        fixture.expose()
    );
}

#[test]
fn every_truncated_prefix_trailing_data_and_oversized_record_is_rejected() {
    let fixture = fixture();
    for length in 0..fixture.len() {
        assert!(
            decode_twitch_session(&SecretValue::from_bytes(
                fixture.expose()[..length].to_vec()
            ))
            .is_err()
        );
    }
    let mut trailing = fixture.expose().to_vec();
    trailing.push(0);
    assert_eq!(
        decode_twitch_session(&SecretValue::from_bytes(trailing)).unwrap_err(),
        TwitchSessionError::CorruptRecord
    );
    assert_eq!(
        decode_twitch_session(&SecretValue::from_bytes(vec![
            0;
            MAX_TWITCH_SESSION_BYTES + 1
        ]))
        .unwrap_err(),
        TwitchSessionError::ValueTooLarge
    );
}

#[test]
fn wire_lengths_scope_count_magic_and_version_are_independently_checked() {
    let fixture = fixture();
    for offset in [20, 24, 28, 32, 36, 65] {
        for value in [0, 2, u32::MAX] {
            let mut bytes = fixture.expose().to_vec();
            bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
            assert_eq!(
                decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
                TwitchSessionError::CorruptRecord
            );
        }
    }
    for offset in 0..8 {
        let mut bytes = fixture.expose().to_vec();
        bytes[offset] ^= 1;
        assert_eq!(
            decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
            TwitchSessionError::CorruptRecord
        );
    }
    for version in [0u32, 2, u32::MAX] {
        let mut bytes = fixture.expose().to_vec();
        bytes[8..12].copy_from_slice(&version.to_le_bytes());
        assert_eq!(
            decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
            TwitchSessionError::UnsupportedVersion
        );
    }
}

#[test]
fn expiry_bounds_and_subsecond_precision_match_v1() {
    for seconds in [1u64, 253_402_300_799] {
        let mut value = session();
        value.credentials.access_expires_at =
            SystemTime::UNIX_EPOCH + Duration::from_secs(seconds) + Duration::from_millis(999);
        let decoded = decode_twitch_session(&encode_twitch_session(&value).unwrap()).unwrap();
        assert_eq!(
            decoded.credentials.access_expires_at,
            SystemTime::UNIX_EPOCH + Duration::from_secs(seconds)
        );
    }
    for seconds in [-1i64, 0, 253_402_300_800, i64::MAX] {
        let mut bytes = fixture().expose().to_vec();
        bytes[12..20].copy_from_slice(&seconds.to_le_bytes());
        assert_eq!(
            decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
            TwitchSessionError::CorruptRecord
        );
    }
    for expiry in [
        SystemTime::UNIX_EPOCH,
        SystemTime::UNIX_EPOCH - Duration::from_secs(1),
        SystemTime::UNIX_EPOCH + Duration::from_secs(253_402_300_800),
    ] {
        let mut value = session();
        value.credentials.access_expires_at = expiry;
        assert_eq!(
            encode_twitch_session(&value).unwrap_err(),
            TwitchSessionError::InvalidSession
        );
    }
}

#[test]
fn tokens_require_bounded_nonempty_visible_ascii_on_both_paths() {
    for token in [
        vec![],
        vec![b'a'; MAX_TWITCH_TOKEN_BYTES + 1],
        b"bad token".to_vec(),
        b"bad\ntoken".to_vec(),
        vec![0],
        vec![0x7f],
        vec![0xff],
    ] {
        for access in [true, false] {
            let mut value = session();
            let secret = SecretValue::from_bytes(token.clone());
            assert!(!valid_twitch_token(&secret));
            if access {
                value.credentials.access_token = secret;
            } else {
                value.credentials.refresh_token = secret;
            }
            assert_eq!(
                encode_twitch_session(&value).unwrap_err(),
                TwitchSessionError::InvalidSession
            );
        }
    }
    for offset in [40, 46] {
        for byte in [b' ', b'\n', 0, 0x7f, 0xff] {
            let mut bytes = fixture().expose().to_vec();
            bytes[offset] = byte;
            assert_eq!(
                decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
                TwitchSessionError::CorruptRecord
            );
        }
    }
    assert!(valid_twitch_token(&SecretValue::from_bytes(
        (0x21..=0x7e).collect()
    )));
}

#[test]
fn canonical_user_id_login_and_exact_scope_are_required() {
    for id in [
        "",
        "0",
        "01",
        "-1",
        "+1",
        "1 ",
        "1x",
        "18446744073709551616",
        "123456789012345678901",
    ] {
        assert!(!valid_twitch_user_id(id));
        let mut value = session();
        value.user_id = id.into();
        assert_eq!(
            encode_twitch_session(&value).unwrap_err(),
            TwitchSessionError::InvalidSession
        );
    }
    assert!(valid_twitch_user_id("18446744073709551615"));
    for login in ["", "Upper", "bad-name", "bad name", "bad\n", "bad\u{80}"] {
        assert!(!valid_twitch_login(login));
        let mut value = session();
        value.login = login.into();
        assert_eq!(
            encode_twitch_session(&value).unwrap_err(),
            TwitchSessionError::InvalidSession
        );
    }
    assert!(valid_twitch_login(&"a".repeat(64)));
    assert!(!valid_twitch_login(&"a".repeat(65)));
    for scopes in [
        vec![],
        vec!["chat:read".into()],
        vec![TWITCH_CHAT_SCOPE.into(), TWITCH_CHAT_SCOPE.into()],
    ] {
        let mut value = session();
        value.scopes = scopes;
        assert_eq!(
            encode_twitch_session(&value).unwrap_err(),
            TwitchSessionError::InvalidSession
        );
    }
    for (offset, replacement) in [(53, b'0'), (53, 0xff), (56, b'T'), (56, 0xff), (69, b'U')] {
        let mut bytes = fixture().expose().to_vec();
        bytes[offset] = replacement;
        assert_eq!(
            decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
            TwitchSessionError::CorruptRecord
        );
    }
}

#[test]
fn combined_record_limit_is_checked_even_when_each_token_fits() {
    let mut value = session();
    let overhead = encode_twitch_session(&value).unwrap().len() - 6 - 7;
    value.credentials.access_token = SecretValue::from_bytes(vec![b'a'; MAX_TWITCH_TOKEN_BYTES]);
    value.credentials.refresh_token = SecretValue::from_bytes(vec![
        b'r';
        MAX_TWITCH_SESSION_BYTES
            - overhead
            - MAX_TWITCH_TOKEN_BYTES
    ]);
    let encoded = encode_twitch_session(&value).unwrap();
    assert_eq!(encoded.len(), MAX_TWITCH_SESSION_BYTES);
    assert!(decode_twitch_session(&encoded).is_ok());
    value.credentials.refresh_token = SecretValue::from_bytes(vec![
        b'r';
        MAX_TWITCH_SESSION_BYTES
            - overhead
            - MAX_TWITCH_TOKEN_BYTES
            + 1
    ]);
    assert_eq!(
        encode_twitch_session(&value).unwrap_err(),
        TwitchSessionError::ValueTooLarge
    );
}

#[test]
fn decoder_rejects_oversized_individual_tokens_inside_a_bounded_record() {
    for access in [true, false] {
        let mut value = session();
        if access {
            value.credentials.access_token =
                SecretValue::from_bytes(vec![b'a'; MAX_TWITCH_TOKEN_BYTES]);
        } else {
            value.credentials.refresh_token =
                SecretValue::from_bytes(vec![b'r'; MAX_TWITCH_TOKEN_BYTES]);
        }
        let mut bytes = encode_twitch_session(&value).unwrap().expose().to_vec();
        let (access_size, refresh_size) = if access {
            (8193u32, 6u32)
        } else {
            (5u32, 8193u32)
        };
        bytes[20..24].copy_from_slice(&access_size.to_le_bytes());
        bytes[24..28].copy_from_slice(&refresh_size.to_le_bytes());
        assert_eq!(
            decode_twitch_session(&SecretValue::from_bytes(bytes)).unwrap_err(),
            TwitchSessionError::CorruptRecord
        );
    }
}

#[test]
fn secret_debug_and_errors_never_expose_session_values() {
    let session = session();
    let encoded = encode_twitch_session(&session).unwrap();
    let debug = format!("{session:?} {:?} {encoded:?}", session.credentials);
    for marker in ["ACCESS", "REFRESH", "test_user", "123"] {
        assert!(!debug.contains(marker));
    }
    for error in [
        TwitchSessionError::InvalidSession,
        TwitchSessionError::ValueTooLarge,
        TwitchSessionError::CorruptRecord,
        TwitchSessionError::UnsupportedVersion,
    ] {
        for marker in ["ACCESS", "REFRESH"] {
            assert!(!format!("{error:?} {}", error.detail()).contains(marker));
        }
    }
}
