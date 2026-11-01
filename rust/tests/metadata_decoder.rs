use manny_uploader::application::{CancellationSource, MetadataError};
use manny_uploader::domain::EncounterMetadata;
use manny_uploader::evtc::decode_metadata;

fn player(address: u64, account: &[u8]) -> [u8; 96] {
    let mut agent = [0; 96];
    agent[..8].copy_from_slice(&address.to_le_bytes());
    agent[28..33].copy_from_slice(b"Hero\0");
    agent[33..33 + account.len()].copy_from_slice(account);
    agent
}

fn npc(address: u64, species: u32) -> [u8; 96] {
    let mut agent = player(address, b"");
    agent[8..12].copy_from_slice(&species.to_le_bytes());
    agent[12..16].copy_from_slice(&u32::MAX.to_le_bytes());
    agent
}

fn event(state: u8, source: u64, destination: u64) -> [u8; 64] {
    let mut record = [0; 64];
    record[8..16].copy_from_slice(&source.to_le_bytes());
    record[16..24].copy_from_slice(&destination.to_le_bytes());
    record[56] = state;
    record
}

fn payload(agents: &[[u8; 96]], events: &[[u8; 64]]) -> Vec<u8> {
    let mut bytes = b"EVTC20261016".to_vec();
    bytes.extend_from_slice(&[1, 123, 0, 0]);
    bytes.extend_from_slice(&(agents.len() as u32).to_le_bytes());
    for agent in agents {
        bytes.extend_from_slice(agent);
    }
    bytes.extend_from_slice(&2u32.to_le_bytes());
    bytes.resize(bytes.len() + 2 * 68, 0);
    for event in events {
        bytes.extend_from_slice(event);
    }
    bytes
}

fn decode(bytes: &[u8]) -> Result<EncounterMetadata, MetadataError> {
    decode_metadata(bytes, &CancellationSource::default().token())
}

fn valid() -> Vec<u8> {
    payload(&[player(42, b":Player.1234")], &[event(13, 42, 0)])
}

#[test]
fn reads_revision_one_with_skills_and_preserves_account_text() {
    let text = ":Joueur.\u{96ea}1234";
    let result = decode(&payload(
        &[player(42, text.as_bytes())],
        &[event(13, 42, 0)],
    ))
    .unwrap();
    assert_eq!(result.boss_id, 123);
    assert_eq!(result.pov_account, text);
    assert_eq!(result.remaining_health_basis_points, None);
}

#[test]
fn all_truncated_prefixes_and_partial_events_are_rejected() {
    let bytes = valid();
    for length in 0..bytes.len() {
        assert!(
            decode(&bytes[..length]).is_err(),
            "accepted prefix {length}"
        );
    }
    let mut bytes = bytes;
    let complete_length = bytes.len();
    bytes.resize(complete_length + 63, 0);
    for length in complete_length + 1..=bytes.len() {
        assert_eq!(decode(&bytes[..length]), Err(MetadataError::MalformedLog));
    }
}

#[test]
fn rejects_invalid_magic_revision_and_excessive_table_counts() {
    let mut bytes = valid();
    bytes[0] = b'X';
    assert_eq!(decode(&bytes), Err(MetadataError::UnsupportedFormat));
    for revision in [0, 2, 255] {
        let mut bytes = valid();
        bytes[12] = revision;
        assert_eq!(decode(&bytes), Err(MetadataError::UnsupportedFormat));
    }
    for count in [100_001u32, u32::MAX] {
        let mut bytes = valid();
        bytes[16..20].copy_from_slice(&count.to_le_bytes());
        assert_eq!(decode(&bytes), Err(MetadataError::MalformedLog));
    }
    for count in [65_536u32, u32::MAX] {
        let mut bytes = valid();
        bytes[116..120].copy_from_slice(&count.to_le_bytes());
        assert_eq!(decode(&bytes), Err(MetadataError::MalformedLog));
    }
}

#[test]
fn missing_unknown_zero_and_nonplayer_pov_are_distinguished_from_bad_data() {
    for bytes in [
        payload(&[player(42, b":Player.1234")], &[]),
        payload(&[player(42, b":Player.1234")], &[event(13, 99, 0)]),
        payload(&[player(0, b":Player.1234")], &[event(13, 0, 0)]),
        payload(&[npc(42, 123)], &[event(13, 42, 0)]),
        payload(&[player(42, b"")], &[event(13, 42, 0)]),
    ] {
        assert_eq!(decode(&bytes), Err(MetadataError::MissingPointOfView));
    }
}

#[test]
fn first_nonzero_pov_wins_and_unrelated_agent_names_are_not_parsed() {
    let mut unrelated = player(7, b"unused");
    unrelated[28..92].fill(255);
    let bytes = payload(
        &[
            unrelated,
            player(42, b":First.1234"),
            player(43, b":Second.1234"),
        ],
        &[event(13, 0, 0), event(13, 42, 0), event(13, 43, 0)],
    );
    assert_eq!(decode(&bytes).unwrap().pov_account, ":First.1234");
}

#[test]
fn account_requires_nul_terminators_and_valid_utf8() {
    let mut unterminated_character = player(42, b":Player.1234");
    unterminated_character[28..92].fill(b'x');
    let mut unterminated_account = player(42, b":Player.1234");
    unterminated_account[33..92].fill(b'x');
    for agent in [
        unterminated_character,
        unterminated_account,
        player(42, &[0xc3, 0x28]),
        player(42, &[0xed, 0xa0, 0x80]),
    ] {
        assert_eq!(
            decode(&payload(&[agent], &[event(13, 42, 0)])),
            Err(MetadataError::MalformedLog)
        );
    }
}

#[test]
fn last_valid_health_update_across_matching_npcs_is_retained() {
    let agents = [
        player(42, b":Player.1234"),
        npc(100, 123),
        npc(101, 0x0001_007b),
        npc(102, 0xffff_007b),
        npc(103, 456),
    ];
    let events = [
        event(13, 42, 0),
        event(8, 100, 9000),
        event(8, 101, 1234),
        event(8, 100, 10001),
        event(8, 102, 100),
        event(8, 103, 100),
        event(8, 42, 100),
    ];
    assert_eq!(
        decode(&payload(&agents, &events))
            .unwrap()
            .remaining_health_basis_points,
        Some(1234)
    );
    for health in [0, 10_000] {
        assert_eq!(
            decode(&payload(
                &agents,
                &[event(8, 100, health), event(13, 42, 0)]
            ))
            .unwrap()
            .remaining_health_basis_points,
            Some(health as u16)
        );
    }
    assert_eq!(
        decode(&payload(
            &agents,
            &[event(13, 42, 0), event(8, 100, u64::MAX)]
        ))
        .unwrap()
        .remaining_health_basis_points,
        None
    );
}

#[test]
fn cancellation_precedes_validation() {
    let source = CancellationSource::default();
    source.request_stop();
    assert_eq!(
        decode_metadata(&[], &source.token()),
        Err(MetadataError::Cancelled)
    );
    assert_eq!(
        decode_metadata(&valid(), &source.token()),
        Err(MetadataError::Cancelled)
    );
}
