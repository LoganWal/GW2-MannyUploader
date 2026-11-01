use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::LogFileIdentity;
use manny_uploader::http::*;

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let directory = std::env::temp_dir().join(format!(
            "manny-rust-body-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&directory).unwrap();
        let path = directory.join("sample-\u{96ea}.zevtc");
        fs::write(&path, b"abcdefgh").unwrap();
        Self(fs::canonicalize(path).unwrap())
    }
    fn identity(&self) -> LogFileIdentity {
        let metadata = fs::metadata(&self.0).unwrap();
        LogFileIdentity {
            canonical_path: self.0.clone(),
            size: metadata.len(),
            last_write_time: metadata.modified().unwrap(),
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn collect(source: &mut dyn HttpBodySource, chunk: usize) -> Vec<u8> {
    let stop = CancellationSource::default();
    let mut buffer = vec![0; chunk];
    let mut result = Vec::new();
    loop {
        let read = source.read(&mut buffer, &stop.token()).unwrap();
        if read == 0 {
            break;
        }
        result.extend_from_slice(&buffer[..read]);
    }
    result
}

#[test]
fn owned_memory_and_secret_sources_stream_without_rewinding() {
    for source in [
        MemoryBodySource::new(b"synthetic-body".to_vec()).unwrap(),
        MemoryBodySource::from_secret(SecretValue::from_bytes(b"synthetic-body".to_vec())).unwrap(),
    ] {
        let mut body = ExactLengthBody::new(Box::new(source)).unwrap();
        assert_eq!(body.content_length(), 14);
        assert!(!body.is_complete());
        assert_eq!(collect(&mut body, 1), b"synthetic-body");
        assert!(body.is_complete());
        assert!(collect(&mut body, 4).is_empty());
    }
    let mut empty = ExactLengthBody::new(Box::new(MemoryBodySource::new(vec![]).unwrap())).unwrap();
    assert!(collect(&mut empty, 1).is_empty());
    assert!(empty.is_complete());
}

#[test]
fn stable_unicode_file_streams_its_exact_identity() {
    let fixture = Fixture::new();
    let source = FileBodySource::new(fixture.identity()).unwrap();
    let mut body = ExactLengthBody::new(Box::new(source)).unwrap();
    assert_eq!(body.content_length(), 8);
    assert_eq!(collect(&mut body, 3), b"abcdefgh");
    assert!(body.is_complete());
    let mut wrong = fixture.identity();
    wrong.size += 1;
    assert!(matches!(
        FileBodySource::new(wrong),
        Err(HttpBodyError {
            code: HttpBodyErrorCode::SourceChanged,
            ..
        })
    ));
    let mut wrong = fixture.identity();
    wrong.last_write_time += Duration::from_secs(1);
    assert!(matches!(
        FileBodySource::new(wrong),
        Err(HttpBodyError {
            code: HttpBodyErrorCode::SourceChanged,
            ..
        })
    ));
    let mut missing = fixture.identity();
    missing.canonical_path.set_extension("missing");
    assert!(matches!(
        FileBodySource::new(missing),
        Err(HttpBodyError {
            code: HttpBodyErrorCode::SourceUnavailable,
            ..
        })
    ));
}

#[test]
fn file_growth_truncation_and_same_size_timestamp_changes_reject_final_bytes() {
    for change in 0..3 {
        let fixture = Fixture::new();
        let mut body = FileBodySource::new(fixture.identity()).unwrap();
        let stop = CancellationSource::default();
        let mut buffer = [0; 4];
        assert_eq!(body.read(&mut buffer, &stop.token()).unwrap(), 4);
        match change {
            0 => fs::OpenOptions::new()
                .append(true)
                .open(&fixture.0)
                .unwrap()
                .write_all(b"changed")
                .unwrap(),
            1 => fs::OpenOptions::new()
                .write(true)
                .open(&fixture.0)
                .unwrap()
                .set_len(4)
                .unwrap(),
            _ => fs::OpenOptions::new()
                .write(true)
                .open(&fixture.0)
                .unwrap()
                .set_modified(SystemTime::UNIX_EPOCH + Duration::from_secs(12345))
                .unwrap(),
        }
        let failed = body.read(&mut buffer, &stop.token()).unwrap_err();
        assert_eq!(failed.code, HttpBodyErrorCode::SourceChanged);
        assert_eq!(body.read(&mut buffer, &stop.token()), Err(failed));
        assert!(!format!("{failed:?}").contains("sample"));
    }
}

struct BadBody {
    declared: u64,
    behavior: u8,
    calls: usize,
}
impl HttpBodySource for BadBody {
    fn content_length(&self) -> u64 {
        if self.behavior == 4 {
            panic!("synthetic length panic");
        }
        self.declared
    }
    fn read(&mut self, destination: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
        self.calls += 1;
        match self.behavior {
            0 => Ok(0),
            1 => Ok(destination.len() + 1),
            2 => Ok(destination.len()),
            3 => panic!("synthetic read panic"),
            _ => Err(HttpBodyError {
                code: HttpBodyErrorCode::ReadFailed,
                system_error: Some(5),
            }),
        }
    }
}

#[test]
fn exact_length_guard_rejects_early_eof_oversized_counts_excess_bytes_and_panics() {
    let stop = CancellationSource::default();
    for (behavior, expected) in [
        (0, HttpBodyErrorCode::ReadFailed),
        (1, HttpBodyErrorCode::ReadFailed),
        (2, HttpBodyErrorCode::ReadFailed),
        (3, HttpBodyErrorCode::Panicked),
        (5, HttpBodyErrorCode::ReadFailed),
    ] {
        let mut body = ExactLengthBody::new(Box::new(BadBody {
            declared: 4,
            behavior,
            calls: 0,
        }))
        .unwrap();
        let failure = body.read(&mut [0; 8], &stop.token()).unwrap_err();
        assert_eq!(failure.code, expected);
        assert!(!body.is_complete());
        assert_eq!(body.read(&mut [0; 8], &stop.token()), Err(failure));
        if behavior == 5 {
            assert_eq!(failure.system_error, Some(5));
        }
    }
    for length in [MAX_HTTP_REQUEST_BODY_BYTES + 1, u64::MAX] {
        assert!(matches!(
            ExactLengthBody::new(Box::new(BadBody {
                declared: length,
                behavior: 0,
                calls: 0
            })),
            Err(HttpBodyError {
                code: HttpBodyErrorCode::ReadFailed,
                ..
            })
        ));
    }
    assert!(matches!(
        ExactLengthBody::new(Box::new(BadBody {
            declared: 4,
            behavior: 4,
            calls: 0
        })),
        Err(HttpBodyError {
            code: HttpBodyErrorCode::Panicked,
            ..
        })
    ));
}

#[test]
fn cancellation_is_checked_before_source_access_and_after_source_reads() {
    let stop = CancellationSource::default();
    stop.request_stop();
    let mut body = ExactLengthBody::new(Box::new(BadBody {
        declared: 4,
        behavior: 3,
        calls: 0,
    }))
    .unwrap();
    assert_eq!(
        body.read(&mut [0; 4], &stop.token()).unwrap_err().code,
        HttpBodyErrorCode::Cancelled
    );
    let fixture = Fixture::new();
    let mut file = FileBodySource::new(fixture.identity()).unwrap();
    assert_eq!(
        file.read(&mut [0; 4], &stop.token()).unwrap_err().code,
        HttpBodyErrorCode::Cancelled
    );
    struct Cancels(CancellationSource);
    impl HttpBodySource for Cancels {
        fn content_length(&self) -> u64 {
            4
        }
        fn read(&mut self, bytes: &mut [u8], _: &StopToken) -> Result<usize, HttpBodyError> {
            self.0.request_stop();
            Ok(bytes.len())
        }
    }
    let source = CancellationSource::default();
    let token = source.token();
    let mut body = ExactLengthBody::new(Box::new(Cancels(source))).unwrap();
    assert_eq!(
        body.read(&mut [0; 4], &token).unwrap_err().code,
        HttpBodyErrorCode::Cancelled
    );
    assert!(!body.is_complete());
}

#[test]
fn zero_capacity_reads_do_not_consume_or_finish_sources() {
    let mut body =
        ExactLengthBody::new(Box::new(MemoryBodySource::new(b"data".to_vec()).unwrap())).unwrap();
    let stop = CancellationSource::default();
    assert_eq!(body.read(&mut [], &stop.token()), Ok(0));
    assert!(!body.is_complete());
    assert_eq!(collect(&mut body, 4), b"data");
    assert!(body.is_complete());
}

#[cfg(unix)]
#[test]
fn symbolic_links_cannot_replace_canonical_body_paths() {
    let fixture = Fixture::new();
    let link = fixture.0.with_extension("link");
    std::os::unix::fs::symlink(&fixture.0, &link).unwrap();
    let mut identity = fixture.identity();
    identity.canonical_path = link;
    assert!(matches!(
        FileBodySource::new(identity),
        Err(HttpBodyError {
            code: HttpBodyErrorCode::SourceUnavailable,
            ..
        })
    ));
}
