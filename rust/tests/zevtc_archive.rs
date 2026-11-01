use std::io::{Cursor, Read, Seek, SeekFrom, Write};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::SystemTime;

use flate2::{Compression, write::DeflateEncoder};
use manny_uploader::application::{CancellationSource, MetadataError};
use manny_uploader::domain::LogFileIdentity;
use manny_uploader::evtc::{ArchiveLimits, ZevtcArchiveReader};

struct Entry<'a> {
    name: &'a str,
    payload: &'a [u8],
    compressed: Vec<u8>,
    method: u16,
    descriptor: bool,
}

impl<'a> Entry<'a> {
    fn stored(name: &'a str, payload: &'a [u8]) -> Self {
        Self {
            name,
            payload,
            compressed: payload.to_vec(),
            method: 0,
            descriptor: false,
        }
    }

    fn deflated(name: &'a str, payload: &'a [u8]) -> Self {
        let mut encoder = DeflateEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(payload).unwrap();
        Self {
            name,
            payload,
            compressed: encoder.finish().unwrap(),
            method: 8,
            descriptor: false,
        }
    }
}

fn put16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}
fn put32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn archive(entries: &[Entry<'_>], zip64: bool) -> Vec<u8> {
    let mut bytes = Vec::new();
    let mut directory = Vec::new();
    for entry in entries {
        let offset = bytes.len();
        let crc = crc32fast::hash(entry.payload);
        let flags = if entry.descriptor { 8 } else { 0 };
        let version = if zip64 { 45 } else { 20 };
        let mut local = [0; 30];
        local[..4].copy_from_slice(b"PK\x03\x04");
        put16(&mut local, 4, version);
        put16(&mut local, 6, flags);
        put16(&mut local, 8, entry.method);
        if !entry.descriptor {
            put32(&mut local, 14, crc);
        }
        let compressed = if zip64 {
            u32::MAX
        } else if entry.descriptor {
            0
        } else {
            entry.compressed.len() as u32
        };
        let uncompressed = if zip64 {
            u32::MAX
        } else if entry.descriptor {
            0
        } else {
            entry.payload.len() as u32
        };
        put32(&mut local, 18, compressed);
        put32(&mut local, 22, uncompressed);
        put16(&mut local, 26, entry.name.len() as u16);
        put16(&mut local, 28, if zip64 { 20 } else { 0 });
        bytes.extend_from_slice(&local);
        bytes.extend_from_slice(entry.name.as_bytes());
        if zip64 {
            bytes.extend_from_slice(&[1, 0, 16, 0]);
            bytes.extend_from_slice(&(entry.payload.len() as u64).to_le_bytes());
            bytes.extend_from_slice(&(entry.compressed.len() as u64).to_le_bytes());
        }
        bytes.extend_from_slice(&entry.compressed);
        if entry.descriptor {
            bytes.extend_from_slice(b"PK\x07\x08");
            bytes.extend_from_slice(&crc.to_le_bytes());
            if zip64 {
                bytes.extend_from_slice(&(entry.compressed.len() as u64).to_le_bytes());
                bytes.extend_from_slice(&(entry.payload.len() as u64).to_le_bytes());
            } else {
                bytes.extend_from_slice(&(entry.compressed.len() as u32).to_le_bytes());
                bytes.extend_from_slice(&(entry.payload.len() as u32).to_le_bytes());
            }
        }
        let mut central = [0; 46];
        central[..4].copy_from_slice(b"PK\x01\x02");
        put16(&mut central, 4, version);
        put16(&mut central, 6, version);
        put16(&mut central, 8, flags);
        put16(&mut central, 10, entry.method);
        put32(&mut central, 16, crc);
        put32(
            &mut central,
            20,
            if zip64 {
                u32::MAX
            } else {
                entry.compressed.len() as u32
            },
        );
        put32(
            &mut central,
            24,
            if zip64 {
                u32::MAX
            } else {
                entry.payload.len() as u32
            },
        );
        put16(&mut central, 28, entry.name.len() as u16);
        put16(&mut central, 30, if zip64 { 28 } else { 0 });
        put32(
            &mut central,
            42,
            if zip64 { u32::MAX } else { offset as u32 },
        );
        directory.extend_from_slice(&central);
        directory.extend_from_slice(entry.name.as_bytes());
        if zip64 {
            directory.extend_from_slice(&[1, 0, 24, 0]);
            directory.extend_from_slice(&(entry.payload.len() as u64).to_le_bytes());
            directory.extend_from_slice(&(entry.compressed.len() as u64).to_le_bytes());
            directory.extend_from_slice(&(offset as u64).to_le_bytes());
        }
    }
    let start = bytes.len();
    bytes.extend_from_slice(&directory);
    if zip64 {
        let zip64_offset = bytes.len();
        let mut end = [0; 56];
        end[..4].copy_from_slice(b"PK\x06\x06");
        put64(&mut end, 4, 44);
        put16(&mut end, 12, 45);
        put16(&mut end, 14, 45);
        put64(&mut end, 24, entries.len() as u64);
        put64(&mut end, 32, entries.len() as u64);
        put64(&mut end, 40, directory.len() as u64);
        put64(&mut end, 48, start as u64);
        bytes.extend_from_slice(&end);
        let mut locator = [0; 20];
        locator[..4].copy_from_slice(b"PK\x06\x07");
        put64(&mut locator, 8, zip64_offset as u64);
        put32(&mut locator, 16, 1);
        bytes.extend_from_slice(&locator);
    }
    let mut end = [0; 22];
    end[..4].copy_from_slice(b"PK\x05\x06");
    put16(
        &mut end,
        8,
        if zip64 {
            u16::MAX
        } else {
            entries.len() as u16
        },
    );
    put16(
        &mut end,
        10,
        if zip64 {
            u16::MAX
        } else {
            entries.len() as u16
        },
    );
    put32(
        &mut end,
        12,
        if zip64 {
            u32::MAX
        } else {
            directory.len() as u32
        },
    );
    put32(&mut end, 16, if zip64 { u32::MAX } else { start as u32 });
    bytes.extend_from_slice(&end);
    bytes
}

fn extract_with(bytes: &[u8], limits: ArchiveLimits) -> Result<Vec<u8>, MetadataError> {
    ZevtcArchiveReader::new(limits)?.extract_from(
        &mut Cursor::new(bytes),
        bytes.len() as u64,
        &CancellationSource::default().token(),
    )
}
fn extract(bytes: &[u8]) -> Result<Vec<u8>, MetadataError> {
    extract_with(bytes, ArchiveLimits::default())
}
fn central(bytes: &[u8]) -> usize {
    bytes
        .windows(4)
        .position(|part| part == b"PK\x01\x02")
        .unwrap()
}

#[test]
fn stored_deflated_zip64_and_descriptors_extract_exact_bytes() {
    for zip64 in [false, true] {
        for deflate in [false, true] {
            for descriptor in [false, true] {
                let mut entry = if deflate {
                    Entry::deflated("nested/REPORT.EvTc", b"EVTC example bytes")
                } else {
                    Entry::stored("nested/REPORT.EvTc", b"EVTC example bytes")
                };
                entry.descriptor = descriptor;
                assert_eq!(
                    extract(&archive(&[entry], zip64)).unwrap(),
                    b"EVTC example bytes"
                );
            }
        }
    }
}

#[test]
fn entry_selection_allows_one_extensionless_file_and_prefers_evtc() {
    assert_eq!(
        extract(&archive(
            &[
                Entry::stored("folder/", b""),
                Entry::stored("nested/20261016-123456", b"payload")
            ],
            false
        ))
        .unwrap(),
        b"payload"
    );
    assert_eq!(
        extract(&archive(
            &[
                Entry::stored("notes.txt", b"notes"),
                Entry::stored("20261016-123456", b"old"),
                Entry::stored("../fight.evtc", b"selected")
            ],
            false
        ))
        .unwrap(),
        b"selected"
    );
    for entries in [
        vec![Entry::stored("notes.txt", b"text")],
        vec![
            Entry::stored("first", b"one"),
            Entry::stored("second", b"two"),
        ],
        vec![
            Entry::stored("a.evtc", b"one"),
            Entry::stored("a.evtc", b"two"),
        ],
        vec![Entry::stored("a.evtc", b"")],
        vec![],
    ] {
        assert_eq!(
            extract(&archive(&entries, false)),
            Err(MetadataError::InvalidArchive)
        );
    }
}

#[test]
fn truncated_archives_crc_corruption_and_local_mismatches_fail() {
    let bytes = archive(&[Entry::deflated("a.evtc", b"abcdefghijk")], false);
    for length in 0..bytes.len() {
        assert!(
            extract(&bytes[..length]).is_err(),
            "accepted truncated archive {length}"
        );
    }
    for offset in [0, 6, 8, 14, 18, 22, 30, 36] {
        let mut corrupt = bytes.clone();
        corrupt[offset] ^= 0xff;
        assert!(
            extract(&corrupt).is_err(),
            "accepted corruption at {offset}"
        );
    }
    let mut corrupt = archive(&[Entry::stored("a.evtc", b"payload")], false);
    corrupt[36] ^= 1;
    assert_eq!(extract(&corrupt), Err(MetadataError::InvalidArchive));
}

#[test]
fn missing_final_deflate_block_is_rejected_even_when_payload_and_crc_match() {
    let mut entry = Entry::stored("a.evtc", b"payload");
    entry.method = 8;
    // A nonfinal stored DEFLATE block with no following final block.
    entry.compressed = vec![0, 7, 0, 248, 255];
    entry.compressed.extend_from_slice(entry.payload);
    assert_eq!(
        extract(&archive(&[entry], false)),
        Err(MetadataError::InvalidArchive)
    );
}

#[test]
fn known_final_deflate_block_is_valid_but_trailing_compressed_data_is_not() {
    let mut entry = Entry::stored("a.evtc", b"payload");
    entry.method = 8;
    entry.compressed = vec![1, 7, 0, 248, 255];
    entry.compressed.extend_from_slice(entry.payload);
    assert_eq!(extract(&archive(&[entry], false)).unwrap(), b"payload");
    let mut entry = Entry::deflated("a.evtc", b"payload");
    entry.compressed.extend_from_slice(b"trailing data");
    assert_eq!(
        extract(&archive(&[entry], false)),
        Err(MetadataError::InvalidArchive)
    );
}

#[test]
fn deterministic_mutations_remain_bounded_and_never_panic() {
    let seeds = [
        archive(&[Entry::stored("a.evtc", b"payload")], false),
        archive(&[Entry::deflated("a.evtc", b"payload")], true),
    ];
    let mut random = 0x1234_5678u32;
    let limits = ArchiveLimits {
        archive_bytes: 4096,
        uncompressed_bytes: 4096,
        ..Default::default()
    };
    for seed in seeds {
        for _ in 0..2000 {
            let mut bytes = seed.clone();
            for _ in 0..3 {
                random = random.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                let offset = random as usize % bytes.len();
                bytes[offset] ^= (random >> 24) as u8;
            }
            if let Ok(output) = extract_with(&bytes, limits) {
                assert!(output.len() <= 4096);
            }
        }
    }
}

#[test]
fn encryption_and_unsupported_methods_are_rejected() {
    for flags in [1, 0x40, 0x2000] {
        let mut bytes = archive(&[Entry::stored("a.evtc", b"payload")], false);
        let cd = central(&bytes);
        put16(&mut bytes, 6, flags);
        put16(&mut bytes, cd + 8, flags);
        assert_eq!(extract(&bytes), Err(MetadataError::InvalidArchive));
    }
    let mut entry = Entry::stored("a.evtc", b"payload");
    entry.method = 12;
    assert_eq!(
        extract(&archive(&[entry], false)),
        Err(MetadataError::InvalidArchive)
    );
}

#[test]
fn resource_limits_are_enforced_before_extraction() {
    let bytes = archive(&[Entry::deflated("a.evtc", &[0; 4096])], false);
    for limits in [
        ArchiveLimits {
            archive_bytes: 1,
            ..Default::default()
        },
        ArchiveLimits {
            uncompressed_bytes: 4095,
            ..Default::default()
        },
        ArchiveLimits {
            filename_bytes: 5,
            ..Default::default()
        },
        ArchiveLimits {
            compression_ratio: 1,
            ..Default::default()
        },
    ] {
        assert_eq!(
            extract_with(&bytes, limits),
            Err(MetadataError::ResourceLimit)
        );
    }
    let entries: Vec<_> = (0..17).map(|_| Entry::stored("folder/", b"")).collect();
    assert_eq!(
        extract(&archive(&entries, false)),
        Err(MetadataError::ResourceLimit)
    );
    for limits in [
        ArchiveLimits {
            archive_bytes: 0,
            ..Default::default()
        },
        ArchiveLimits {
            uncompressed_bytes: 0,
            ..Default::default()
        },
        ArchiveLimits {
            entries: 0,
            ..Default::default()
        },
        ArchiveLimits {
            filename_bytes: 0,
            ..Default::default()
        },
        ArchiveLimits {
            compression_ratio: 0,
            ..Default::default()
        },
    ] {
        assert!(matches!(
            ZevtcArchiveReader::new(limits),
            Err(MetadataError::ResourceLimit)
        ));
    }
}

#[test]
fn hostile_zip64_offsets_counts_and_sizes_fail_without_large_allocations() {
    let original = archive(&[Entry::stored("a.evtc", b"payload")], true);
    let end = original.len() - 22 - 20 - 56;
    for (offset, value) in [
        (end + 48, u64::MAX),
        (end + 40, u64::MAX),
        (original.len() - 22 - 20 + 8, u64::MAX),
    ] {
        let mut bytes = original.clone();
        put64(&mut bytes, offset, value);
        assert!(extract(&bytes).is_err());
    }
    let mut bytes = original.clone();
    put64(&mut bytes, end + 24, u64::MAX);
    put64(&mut bytes, end + 32, u64::MAX);
    assert_eq!(extract(&bytes), Err(MetadataError::ResourceLimit));
    let mut bytes = original;
    let cd = central(&bytes);
    put64(&mut bytes, cd + 46 + 6 + 4, u64::MAX);
    assert_eq!(extract(&bytes), Err(MetadataError::ResourceLimit));
}

#[test]
fn unsigned_descriptors_are_supported_and_bad_descriptors_fail() {
    let mut entry = Entry::stored("a.evtc", b"payload");
    entry.descriptor = true;
    let mut bytes = archive(&[entry], false);
    let descriptor = 30 + 6 + 7;
    let cd = central(&bytes);
    let mut invalid = bytes.clone();
    invalid[descriptor + 4] ^= 1;
    assert_eq!(extract(&invalid), Err(MetadataError::InvalidArchive));
    bytes.drain(descriptor..descriptor + 4);
    let eocd = bytes.len() - 22;
    put32(&mut bytes, eocd + 16, (cd - 4) as u32);
    assert_eq!(extract(&bytes).unwrap(), b"payload");
}

#[test]
fn zip64_local_descriptor_can_have_small_central_sizes() {
    let mut entry = Entry::stored("a.evtc", b"payload");
    entry.descriptor = true;
    let mut bytes = archive(&[entry], true);
    let cd = central(&bytes);
    put32(&mut bytes, cd + 20, 7);
    put32(&mut bytes, cd + 24, 7);
    put32(&mut bytes, cd + 42, 0);
    assert_eq!(extract(&bytes).unwrap(), b"payload");
}

struct CancellingReader {
    reader: Cursor<Vec<u8>>,
    cancel_at: u64,
    source: Arc<CancellationSource>,
}
impl Read for CancellingReader {
    fn read(&mut self, buffer: &mut [u8]) -> std::io::Result<usize> {
        if self.reader.position() == self.cancel_at {
            self.source.request_stop();
        }
        self.reader.read(buffer)
    }
}
impl Seek for CancellingReader {
    fn seek(&mut self, position: SeekFrom) -> std::io::Result<u64> {
        self.reader.seek(position)
    }
}

#[test]
fn cancellation_is_checked_before_io_and_between_output_chunks() {
    let bytes = archive(&[Entry::stored("a.evtc", &vec![1; 128 * 1024])], false);
    let size = bytes.len() as u64;
    let source = Arc::new(CancellationSource::default());
    let mut reader = CancellingReader {
        reader: Cursor::new(bytes),
        cancel_at: 36,
        source: source.clone(),
    };
    let archive = ZevtcArchiveReader::new(ArchiveLimits::default()).unwrap();
    assert_eq!(
        archive.extract_from(&mut reader, size, &source.token()),
        Err(MetadataError::Cancelled)
    );
    reader.reader.set_position(123);
    assert_eq!(
        archive.extract_from(&mut reader, size, &source.token()),
        Err(MetadataError::Cancelled)
    );
    assert_eq!(reader.reader.position(), 123);
}

#[test]
fn unicode_filesystem_reader_validates_identity_and_decodes_metadata() {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let path = std::env::temp_dir().join(format!(
        "manny-rust-{}-{}-\u{96ea}.zevtc",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    struct Cleanup(std::path::PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let _cleanup = Cleanup(path.clone());
    let mut payload = vec![0; 184];
    payload[..4].copy_from_slice(b"EVTC");
    payload[12] = 1;
    put16(&mut payload, 13, 123);
    put32(&mut payload, 16, 1);
    put64(&mut payload, 20, 42);
    payload[48..67].copy_from_slice(b"Hero\0:Player.1234\0\0");
    put64(&mut payload, 128, 42);
    payload[176] = 13;
    let bytes = archive(&[Entry::deflated("20261016-123456", &payload)], false);
    std::fs::write(&path, &bytes).unwrap();
    let mut identity = LogFileIdentity {
        canonical_path: path,
        size: bytes.len() as u64,
        last_write_time: SystemTime::UNIX_EPOCH,
    };
    let reader = ZevtcArchiveReader::new(ArchiveLimits::default()).unwrap();
    assert_eq!(
        reader
            .parse(&identity, &CancellationSource::default().token())
            .unwrap()
            .pov_account,
        ":Player.1234"
    );
    identity.size += 1;
    assert_eq!(
        reader.extract(&identity, &CancellationSource::default().token()),
        Err(MetadataError::FileUnavailable)
    );
}
