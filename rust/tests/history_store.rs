use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use manny_uploader::application::*;
use manny_uploader::config::*;
use manny_uploader::domain::*;

#[derive(Default)]
struct Memory {
    bytes: Option<Vec<u8>>,
    read_error: Option<FileError>,
    write_error: Option<FileError>,
    reads: usize,
    writes: usize,
}
struct Files(Arc<Mutex<Memory>>);
impl FileStore for Files {
    fn read(&self, _: &Path, limit: usize) -> Result<Option<Vec<u8>>, FileError> {
        assert_eq!(limit, MAX_HISTORY_BYTES);
        let mut state = self.0.lock().unwrap();
        state.reads += 1;
        if let Some(error) = state.read_error {
            return Err(error);
        }
        if state
            .bytes
            .as_ref()
            .is_some_and(|bytes| bytes.len() > limit)
        {
            return Err(FileError::TooLarge);
        }
        Ok(state.bytes.clone())
    }
    fn write_atomic(&self, _: &Path, bytes: &[u8]) -> Result<(), FileError> {
        let mut state = self.0.lock().unwrap();
        state.writes += 1;
        if let Some(error) = state.write_error {
            return Err(error);
        }
        state.bytes = Some(bytes.into());
        Ok(())
    }
    fn erase(&self, _: &Path) -> Result<(), FileError> {
        panic!("history must not erase files")
    }
}
fn record(size: u64) -> UploadJobRecord {
    let mut record = decode_history(include_bytes!("fixtures/history-v1.json"), 10)
        .unwrap()
        .remove(0);
    record.file.size = size;
    record
}
fn open(memory: &Arc<Mutex<Memory>>, capacity: usize) -> JsonUploadHistoryStore {
    JsonUploadHistoryStore::new(
        Box::new(Files(memory.clone())),
        "nested/history.json".into(),
        capacity,
    )
    .unwrap()
}

#[test]
fn creation_validates_before_io_and_missing_history_is_empty() {
    let memory = Arc::new(Mutex::new(Memory::default()));
    for (path, capacity) in [
        ("", 10),
        ("directory/", 10),
        ("directory/.", 10),
        ("history.json", 0),
        ("history.json", 10001),
    ] {
        assert!(matches!(
            JsonUploadHistoryStore::new(Box::new(Files(memory.clone())), path.into(), capacity),
            Err(HistoryStoreError::InvalidConfiguration)
        ));
    }
    assert_eq!(memory.lock().unwrap().reads, 0);
    let store = open(&memory, 10);
    assert!(store.records().is_empty());
    assert_eq!(store.recovery(), None);
    assert_eq!(store.path(), PathBuf::from("nested/history.json"));
    assert_eq!(memory.lock().unwrap().writes, 0);
}

#[test]
fn merge_preserves_hidden_rows_replaces_in_place_and_trims_oldest() {
    let memory = Arc::new(Mutex::new(Memory::default()));
    let mut store = open(&memory, 3);
    store
        .merge_and_save(&[record(1), record(2), record(3)])
        .unwrap();
    let mut updated = record(1);
    updated.providers[0].attempts += 1;
    updated.providers[0].detail = "Explicit upload completed".into();
    store.merge_and_save(&[updated.clone()]).unwrap();
    assert_eq!(store.records(), &[updated, record(2), record(3)]);
    let mut reopened = open(&memory, 3);
    assert_eq!(reopened.records(), store.records());
    reopened.merge_and_save(&[]).unwrap();
    assert_eq!(reopened.records(), store.records());
    reopened.merge_and_save(&[record(4)]).unwrap();
    assert_eq!(reopened.records(), &[record(2), record(3), record(4)]);
    assert_eq!(open(&memory, 3).records(), reopened.records());
}

#[test]
fn invalid_and_unreadable_history_is_visible_and_not_replaced_until_valid_save() {
    let memory = Arc::new(Mutex::new(Memory::default()));
    for bytes in [
        b"invalid".to_vec(),
        br#"{"schema_version":2,"logs":[]}"#.to_vec(),
        encode_history(&[record(1), record(2)], 2).unwrap(),
    ] {
        memory.lock().unwrap().bytes = Some(bytes.clone());
        let mut store = open(&memory, 1);
        assert_eq!(store.recovery(), Some(HistoryRecovery::InvalidDocument));
        assert!(store.records().is_empty());
        assert_eq!(memory.lock().unwrap().bytes, Some(bytes));
        store.merge_and_save(&[record(3)]).unwrap();
        assert_eq!(store.recovery(), None);
        assert_eq!(open(&memory, 1).records(), &[record(3)]);
    }
    for error in [
        FileError::ReadFailed,
        FileError::TooLarge,
        FileError::ChangedDuringRead,
    ] {
        memory.lock().unwrap().read_error = Some(error);
        let store = open(&memory, 1);
        assert!(store.records().is_empty());
        assert_eq!(store.recovery(), Some(HistoryRecovery::File(error)));
    }
}

#[test]
fn failed_writes_preserve_memory_file_and_recovery_then_allow_retry() {
    let memory = Arc::new(Mutex::new(Memory::default()));
    let mut store = open(&memory, 2);
    store.merge_and_save(&[record(1)]).unwrap();
    let previous = memory.lock().unwrap().bytes.clone();
    for error in [
        FileError::WriteFailed,
        FileError::FlushFailed,
        FileError::ReplaceFailed,
    ] {
        memory.lock().unwrap().write_error = Some(error);
        assert_eq!(
            store.merge_and_save(&[record(2)]),
            Err(HistoryStoreError::File(error))
        );
        assert_eq!(store.records(), &[record(1)]);
        assert_eq!(memory.lock().unwrap().bytes, previous);
    }
    memory.lock().unwrap().bytes = Some(b"invalid".to_vec());
    let mut recovering = open(&memory, 2);
    assert!(recovering.merge_and_save(&[record(2)]).is_err());
    assert_eq!(
        recovering.recovery(),
        Some(HistoryRecovery::InvalidDocument)
    );
    assert!(recovering.records().is_empty());
    memory.lock().unwrap().write_error = None;
    recovering.merge_and_save(&[record(2)]).unwrap();
    assert_eq!(recovering.recovery(), None);
    assert_eq!(open(&memory, 2).records(), &[record(2)]);
}

#[test]
fn invalid_updates_fail_before_any_write_even_if_they_would_be_trimmed() {
    let memory = Arc::new(Mutex::new(Memory::default()));
    let mut store = open(&memory, 1);
    store.merge_and_save(&[record(1)]).unwrap();
    let previous = memory.lock().unwrap().bytes.clone();
    let mut invalid = record(2);
    invalid.donbot_upload_receipt.as_mut().unwrap().fight_log_id = Some(i64::MAX as u64 + 1);
    assert_eq!(
        store.merge_and_save(&[invalid, record(3)]),
        Err(HistoryStoreError::InvalidRecord)
    );
    assert_eq!(
        store.merge_and_save(&[record(2), record(2)]),
        Err(HistoryStoreError::DuplicateIdentity)
    );
    assert_eq!(store.records(), &[record(1)]);
    assert_eq!(memory.lock().unwrap().bytes, previous);
    assert_eq!(memory.lock().unwrap().writes, 1);
}

#[test]
fn saved_busy_work_normalizes_only_on_reopen() {
    let memory = Arc::new(Mutex::new(Memory::default()));
    let mut store = open(&memory, 2);
    let mut active = record(1);
    active.providers[0].state = ProviderState::Active;
    store.merge_and_save(&[active.clone()]).unwrap();
    assert_eq!(store.records(), &[active.clone()]);
    let reopened = open(&memory, 2);
    active.providers[0].state = ProviderState::Failed;
    active.providers[0].detail = "Interrupted by the previous game session".into();
    assert_eq!(reopened.records(), &[active]);
}
