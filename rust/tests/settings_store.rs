use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use manny_uploader::application::*;
use manny_uploader::config::{JsonSettingsStore, MAX_SETTINGS_BYTES, decode_settings};
use manny_uploader::domain::Settings;

#[derive(Default)]
struct Memory {
    files: BTreeMap<PathBuf, Vec<u8>>,
    reads: BTreeMap<PathBuf, FileError>,
    fail_write: Option<PathBuf>,
    writes: Vec<PathBuf>,
}
struct Files(Arc<Mutex<Memory>>);
impl FileStore for Files {
    fn read(&self, path: &Path, max: usize) -> Result<Option<Vec<u8>>, FileError> {
        let memory = self.0.lock().unwrap();
        if let Some(error) = memory.reads.get(path) {
            return Err(*error);
        }
        match memory.files.get(path) {
            Some(bytes) if bytes.len() > max => Err(FileError::TooLarge),
            bytes => Ok(bytes.cloned()),
        }
    }
    fn write_atomic(&self, path: &Path, bytes: &[u8]) -> Result<(), FileError> {
        let mut memory = self.0.lock().unwrap();
        memory.writes.push(path.into());
        if memory.fail_write.as_deref() == Some(path) {
            return Err(FileError::ReplaceFailed);
        }
        memory.files.insert(path.into(), bytes.to_vec());
        Ok(())
    }
    fn erase(&self, _: &Path) -> Result<(), FileError> {
        panic!("settings must never erase files")
    }
}

fn settings(limit: u32) -> Settings {
    let mut settings = Settings::new("D:/logs/\u{96ea}".into());
    settings.general.recent_log_limit = limit;
    settings
}
fn fixture() -> (JsonSettingsStore, Arc<Mutex<Memory>>) {
    let memory = Arc::new(Mutex::new(Memory::default()));
    let store = JsonSettingsStore::new(
        Box::new(Files(memory.clone())),
        "settings.json".into(),
        settings(100),
    )
    .unwrap();
    (store, memory)
}

#[test]
fn defaults_require_two_missing_files_and_creation_validates_configuration() {
    let (store, memory) = fixture();
    let loaded = store.load().unwrap();
    assert_eq!(loaded.source, SettingsLoadSource::Defaults);
    assert_eq!(loaded.settings, settings(100));
    assert!(loaded.recovery.is_none() && memory.lock().unwrap().writes.is_empty());
    for path in ["", ".", "directory/.", "directory/"] {
        assert!(matches!(
            JsonSettingsStore::new(Box::new(Files(memory.clone())), path.into(), settings(100)),
            Err(SettingsStoreError::InvalidConfiguration)
        ));
    }
    assert!(matches!(
        JsonSettingsStore::new(
            Box::new(Files(memory.clone())),
            "settings.json".into(),
            settings(0)
        ),
        Err(SettingsStoreError::InvalidValues(_))
    ));
    memory
        .lock()
        .unwrap()
        .files
        .insert(store.primary_path().into(), b"invalid".to_vec());
    assert_eq!(
        store.load(),
        Err(SettingsStoreError::NoValidSettings {
            primary: SettingsReadIssue::InvalidJson,
            backup: SettingsReadIssue::Missing
        })
    );
    memory.lock().unwrap().files.remove(store.primary_path());
    memory
        .lock()
        .unwrap()
        .files
        .insert(store.backup_path().into(), b"invalid".to_vec());
    assert_eq!(
        store.load(),
        Err(SettingsStoreError::NoValidSettings {
            primary: SettingsReadIssue::Missing,
            backup: SettingsReadIssue::InvalidJson
        })
    );
}

#[test]
fn backup_retains_exact_previous_document_and_recovery_is_explicit() {
    let (store, memory) = fixture();
    store.save(&settings(10)).unwrap();
    let first = memory.lock().unwrap().files[store.primary_path()].clone();
    assert!(
        !memory
            .lock()
            .unwrap()
            .files
            .contains_key(store.backup_path())
    );
    store.save(&settings(20)).unwrap();
    assert_eq!(memory.lock().unwrap().files[store.backup_path()], first);
    assert_eq!(store.load().unwrap().settings, settings(20));
    memory
        .lock()
        .unwrap()
        .files
        .insert(store.primary_path().into(), b"corrupt".to_vec());
    let recovered = store.load().unwrap();
    assert_eq!(recovered.source, SettingsLoadSource::Backup);
    assert_eq!(recovered.settings, settings(10));
    assert_eq!(recovered.recovery, Some(SettingsReadIssue::InvalidJson));
    store.save(&settings(30)).unwrap();
    assert_eq!(memory.lock().unwrap().files[store.backup_path()], first);
    memory.lock().unwrap().files.remove(store.primary_path());
    assert_eq!(
        store.load().unwrap().recovery,
        Some(SettingsReadIssue::Missing)
    );
    assert_eq!(store.load().unwrap().settings, settings(10));
}

#[test]
fn invalid_or_oversized_primary_never_overwrites_last_known_good_backup() {
    let (store, memory) = fixture();
    store.save(&settings(10)).unwrap();
    store.save(&settings(20)).unwrap();
    let backup = memory.lock().unwrap().files[store.backup_path()].clone();
    for corrupt in [
        vec![b' '; MAX_SETTINGS_BYTES + 1],
        br#"{"schema_version":2}"#.to_vec(),
        b"{truncated".to_vec(),
    ] {
        memory
            .lock()
            .unwrap()
            .files
            .insert(store.primary_path().into(), corrupt);
        assert_eq!(store.load().unwrap().source, SettingsLoadSource::Backup);
        store.save(&settings(30)).unwrap();
        assert_eq!(memory.lock().unwrap().files[store.backup_path()], backup);
        assert_eq!(store.load().unwrap().settings, settings(30));
    }
}

#[test]
fn validation_and_read_failures_do_not_write_and_valid_backup_can_recover_read_error() {
    let (store, memory) = fixture();
    store.save(&settings(10)).unwrap();
    store.save(&settings(20)).unwrap();
    memory.lock().unwrap().writes.clear();
    assert!(matches!(
        store.save(&settings(0)),
        Err(SettingsStoreError::InvalidValues(_))
    ));
    assert!(memory.lock().unwrap().writes.is_empty());
    memory
        .lock()
        .unwrap()
        .reads
        .insert(store.primary_path().into(), FileError::ReadFailed);
    assert_eq!(
        store.save(&settings(30)),
        Err(SettingsStoreError::File(FileError::ReadFailed))
    );
    assert!(memory.lock().unwrap().writes.is_empty());
    let recovered = store.load().unwrap();
    assert_eq!(recovered.settings, settings(10));
    assert_eq!(
        recovered.recovery,
        Some(SettingsReadIssue::File(FileError::ReadFailed))
    );
    memory
        .lock()
        .unwrap()
        .reads
        .insert(store.backup_path().into(), FileError::ReadFailed);
    assert!(matches!(
        store.load(),
        Err(SettingsStoreError::NoValidSettings { .. })
    ));
}

#[test]
fn backup_failure_prevents_primary_replacement_and_primary_failure_leaves_backup_valid() {
    let (store, memory) = fixture();
    store.save(&settings(10)).unwrap();
    store.save(&settings(20)).unwrap();
    memory.lock().unwrap().writes.clear();
    memory.lock().unwrap().fail_write = Some(store.backup_path().into());
    assert_eq!(
        store.save(&settings(30)),
        Err(SettingsStoreError::File(FileError::ReplaceFailed))
    );
    assert_eq!(
        memory.lock().unwrap().writes,
        vec![store.backup_path().to_path_buf()]
    );
    assert_eq!(store.load().unwrap().settings, settings(20));
    assert_eq!(
        decode_settings(&memory.lock().unwrap().files[store.backup_path()]).unwrap(),
        settings(10)
    );
    memory.lock().unwrap().writes.clear();
    memory.lock().unwrap().fail_write = Some(store.primary_path().into());
    assert_eq!(
        store.save(&settings(30)),
        Err(SettingsStoreError::File(FileError::ReplaceFailed))
    );
    assert_eq!(
        memory.lock().unwrap().writes,
        vec![
            store.backup_path().to_path_buf(),
            store.primary_path().to_path_buf()
        ]
    );
    assert_eq!(store.load().unwrap().settings, settings(20));
    assert_eq!(
        decode_settings(&memory.lock().unwrap().files[store.backup_path()]).unwrap(),
        settings(20)
    );
}
