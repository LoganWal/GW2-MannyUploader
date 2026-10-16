use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use manny_uploader::application::*;
use manny_uploader::config::ProtectedFileSecretStore;

const MARKER: &[u8] = b"synthetic-credential-marker";
fn value() -> SecretValue {
    SecretValue::from_bytes(MARKER.to_vec())
}

#[derive(Default)]
struct State {
    files: BTreeMap<PathBuf, Vec<u8>>,
    read_error: Option<FileError>,
    write_error: Option<FileError>,
    erase_error: Option<FileError>,
    protect_error: Option<SecretProtectionError>,
    unprotect_error: Option<SecretProtectionError>,
    protected_output_size: Option<usize>,
    reads: usize,
    writes: usize,
    erases: usize,
    protects: usize,
    unprotects: usize,
}
struct Files(Arc<Mutex<State>>);
impl FileStore for Files {
    fn read(&self, path: &Path, maximum: usize) -> Result<Option<Vec<u8>>, FileError> {
        assert_eq!(maximum, MAX_PROTECTED_RECORD_BYTES);
        let mut state = self.0.lock().unwrap();
        state.reads += 1;
        if let Some(error) = state.read_error {
            return Err(error);
        }
        if state.files.get(path).is_some_and(|v| v.len() > maximum) {
            return Err(FileError::TooLarge);
        }
        Ok(state.files.get(path).cloned())
    }
    fn write_atomic(&self, path: &Path, bytes: &[u8]) -> Result<(), FileError> {
        let mut state = self.0.lock().unwrap();
        state.writes += 1;
        if let Some(error) = state.write_error {
            return Err(error);
        }
        state.files.insert(path.into(), bytes.into());
        Ok(())
    }
    fn erase(&self, path: &Path) -> Result<(), FileError> {
        let mut state = self.0.lock().unwrap();
        state.erases += 1;
        if let Some(error) = state.erase_error {
            return Err(error);
        }
        state.files.remove(path);
        let mut temp = path.as_os_str().to_os_string();
        temp.push(".tmp");
        state.files.remove(Path::new(&temp));
        Ok(())
    }
}

// Test-only reversible transform for inspecting envelope bytes. Never used by production adapters.
fn transform(bytes: &[u8]) -> Vec<u8> {
    bytes.iter().map(|b| b ^ 0xa5).collect()
}
struct Protector(Arc<Mutex<State>>);
impl SecretProtector for Protector {
    fn mode(&self) -> SecretProtectionMode {
        SecretProtectionMode::WineCompatibility
    }
    fn protect(&self, bytes: &[u8]) -> Result<Vec<u8>, SecretProtectionError> {
        let mut state = self.0.lock().unwrap();
        state.protects += 1;
        if let Some(error) = state.protect_error {
            return Err(error);
        }
        if let Some(size) = state.protected_output_size {
            return Ok(vec![0xa5; size]);
        }
        Ok(transform(bytes))
    }
    fn unprotect(&self, bytes: &[u8]) -> Result<SecretValue, SecretProtectionError> {
        let mut state = self.0.lock().unwrap();
        state.unprotects += 1;
        if let Some(error) = state.unprotect_error {
            return Err(error);
        }
        Ok(SecretValue::from_bytes(transform(bytes)))
    }
}
fn fixture() -> (ProtectedFileSecretStore, Arc<Mutex<State>>) {
    let state = Arc::new(Mutex::new(State::default()));
    let store = ProtectedFileSecretStore::new(
        Box::new(Files(state.clone())),
        Box::new(Protector(state.clone())),
        "synthetic/credentials".into(),
    )
    .unwrap();
    (store, state)
}
fn code(result: Result<SecretValue, SecretStoreError>) -> SecretStoreErrorCode {
    result.unwrap_err().code
}

#[test]
fn values_are_explicitly_exposed_move_owned_and_always_redacted() {
    let original = value();
    assert_eq!(original.len(), MARKER.len());
    assert_eq!(format!("{original:?}"), "SecretValue([REDACTED])");
    let mut moved = original;
    assert_eq!(moved.expose(), MARKER);
    moved.clear();
    assert!(moved.is_empty());
    assert_eq!(format!("{moved:?}"), "SecretValue([REDACTED])");
    assert!(SecretValue::default().is_empty());
    for (index, id) in SecretId::ALL.into_iter().enumerate() {
        assert_eq!(SecretId::try_from(index as u32 + 1), Ok(id));
    }
    for invalid in [0, 4, 255, u32::MAX] {
        assert_eq!(
            SecretId::try_from(invalid).unwrap_err().code,
            SecretStoreErrorCode::InvalidId
        );
    }
}

#[test]
fn constructor_missing_load_and_filenames_have_no_caller_secret_text() {
    let (store, state) = fixture();
    for directory in ["", "invalid\0directory"] {
        assert!(matches!(
            ProtectedFileSecretStore::new(
                Box::new(Files(state.clone())),
                Box::new(Protector(state.clone())),
                directory.into()
            ),
            Err(SecretStoreError {
                code: SecretStoreErrorCode::InvalidConfiguration,
                ..
            })
        ));
    }
    assert_eq!(state.lock().unwrap().reads, 0);
    assert_eq!(store.mode(), SecretProtectionMode::WineCompatibility);
    for (id, name) in SecretId::ALL.into_iter().zip([
        "dps-report-token.bin",
        "donbot-gw2-api-key.bin",
        "twitch-oauth-session.bin",
    ]) {
        assert_eq!(store.record_path(id), store.directory().join(name));
        let error = store.load(id).unwrap_err();
        assert_eq!(error.code, SecretStoreErrorCode::NotFound);
        assert_eq!(error.id, Some(id));
        assert!(!error.to_string().contains("synthetic"));
    }
    assert_eq!(state.lock().unwrap().unprotects, 0);
}

#[test]
fn every_id_round_trips_replaces_atomically_and_erases_without_backups() {
    let (mut store, state) = fixture();
    for id in SecretId::ALL {
        store.store(id, &value()).unwrap();
        assert_eq!(store.load(id).unwrap().expose(), MARKER);
        let path = store.record_path(id);
        let bytes = state.lock().unwrap().files[&path].clone();
        assert!(!bytes.windows(MARKER.len()).any(|window| window == MARKER));
        let envelope = transform(&bytes);
        assert_eq!(&envelope[..8], b"MNYSECR1");
        assert_eq!(&envelope[8..12], &1_u32.to_le_bytes());
        assert_eq!(&envelope[12..16], &(id as u32).to_le_bytes());
        assert_eq!(&envelope[16..20], &(MARKER.len() as u32).to_le_bytes());
        assert_eq!(&envelope[20..20 + MARKER.len()], MARKER);
        assert_eq!(
            &envelope[20 + MARKER.len()..],
            &crc32fast::hash(&envelope[..20 + MARKER.len()]).to_le_bytes()
        );
        let replacement = SecretValue::from_bytes(vec![0, 0xff, 42]);
        store.store(id, &replacement).unwrap();
        assert_eq!(store.load(id).unwrap().expose(), replacement.expose());
        assert_eq!(state.lock().unwrap().files.len(), 1);
        let mut temporary = path.as_os_str().to_os_string();
        temporary.push(".tmp");
        state.lock().unwrap().files.insert(temporary.into(), bytes);
        store.erase(id).unwrap();
        store.erase(id).unwrap();
        assert!(state.lock().unwrap().files.is_empty());
        assert_eq!(code(store.load(id)), SecretStoreErrorCode::NotFound);
    }
}

#[test]
fn bounds_reject_before_platform_or_file_work_and_accept_maximum_payload() {
    let (mut store, state) = fixture();
    let id = SecretId::DonBotGw2ApiKey;
    assert_eq!(
        store.store(id, &SecretValue::default()).unwrap_err().code,
        SecretStoreErrorCode::EmptySecret
    );
    assert_eq!(
        store
            .store(id, &SecretValue::from_bytes(vec![1; MAX_SECRET_BYTES + 1]))
            .unwrap_err()
            .code,
        SecretStoreErrorCode::SecretTooLarge
    );
    assert_eq!(state.lock().unwrap().protects, 0);
    let maximum = SecretValue::from_bytes(vec![0xff; MAX_SECRET_BYTES]);
    store.store(id, &maximum).unwrap();
    assert_eq!(store.load(id).unwrap().expose(), maximum.expose());
    let previous = state.lock().unwrap().files.clone();
    for size in [0, MAX_PROTECTED_RECORD_BYTES + 1] {
        state.lock().unwrap().protected_output_size = Some(size);
        assert_eq!(
            store.store(id, &value()).unwrap_err().code,
            SecretStoreErrorCode::FileTooLarge
        );
        assert_eq!(state.lock().unwrap().files, previous);
    }
    assert_eq!(state.lock().unwrap().writes, 1);
    let calls = state.lock().unwrap().unprotects;
    for (bytes, expected) in [
        (vec![], SecretStoreErrorCode::CorruptRecord),
        (
            vec![0; MAX_PROTECTED_RECORD_BYTES + 1],
            SecretStoreErrorCode::FileTooLarge,
        ),
    ] {
        state
            .lock()
            .unwrap()
            .files
            .insert(store.record_path(id), bytes);
        assert_eq!(code(store.load(id)), expected);
    }
    assert_eq!(state.lock().unwrap().unprotects, calls);
}

#[test]
fn truncated_tampered_substituted_and_wrong_version_envelopes_fail_closed() {
    let (mut store, state) = fixture();
    let id = SecretId::DpsReportUserToken;
    store.store(id, &value()).unwrap();
    let path = store.record_path(id);
    let ciphertext = state.lock().unwrap().files[&path].clone();
    for length in 0..ciphertext.len() {
        state
            .lock()
            .unwrap()
            .files
            .insert(path.clone(), ciphertext[..length].into());
        assert_eq!(code(store.load(id)), SecretStoreErrorCode::CorruptRecord);
    }
    for offset in 0..ciphertext.len() {
        let mut tampered = ciphertext.clone();
        tampered[offset] ^= 1;
        state.lock().unwrap().files.insert(path.clone(), tampered);
        assert_eq!(code(store.load(id)), SecretStoreErrorCode::CorruptRecord);
    }
    for (offset, number) in [(8, 2_u32), (12, 0), (12, 4), (16, 0), (16, u32::MAX)] {
        let mut envelope = transform(&ciphertext);
        envelope[offset..offset + 4].copy_from_slice(&number.to_le_bytes());
        let end = envelope.len() - 4;
        let checksum = crc32fast::hash(&envelope[..end]);
        envelope[end..].copy_from_slice(&checksum.to_le_bytes());
        state
            .lock()
            .unwrap()
            .files
            .insert(path.clone(), transform(&envelope));
        assert_eq!(code(store.load(id)), SecretStoreErrorCode::CorruptRecord);
    }
    state
        .lock()
        .unwrap()
        .files
        .insert(store.record_path(SecretId::TwitchOAuthSession), ciphertext);
    assert_eq!(
        code(store.load(SecretId::TwitchOAuthSession)),
        SecretStoreErrorCode::CorruptRecord
    );
    state
        .lock()
        .unwrap()
        .files
        .insert(path, vec![0; MAX_SECRET_BYTES + 25]);
    assert_eq!(code(store.load(id)), SecretStoreErrorCode::CorruptRecord);
}

#[test]
fn protector_failures_retain_only_typed_metadata_and_do_not_write() {
    let (mut store, state) = fixture();
    let id = SecretId::TwitchOAuthSession;
    store.store(id, &value()).unwrap();
    let previous = state.lock().unwrap().files.clone();
    for failure in [
        SecretProtectionErrorCode::UnsupportedEnvironment,
        SecretProtectionErrorCode::InputTooLarge,
        SecretProtectionErrorCode::ProtectionFailed,
        SecretProtectionErrorCode::UnprotectionFailed,
        SecretProtectionErrorCode::OutputTooLarge,
    ] {
        let source = SecretProtectionError {
            code: failure,
            system_error: Some(1234),
        };
        state.lock().unwrap().protect_error = Some(source);
        let error = store.store(id, &value()).unwrap_err();
        assert_eq!(
            error,
            SecretStoreError {
                id: Some(id),
                code: SecretStoreErrorCode::Protection(failure),
                system_error: Some(1234)
            }
        );
        assert!(!format!("{error:?} {error}").contains("synthetic"));
        state.lock().unwrap().unprotect_error = Some(source);
        assert_eq!(store.load(id).unwrap_err(), error);
    }
    assert_eq!(state.lock().unwrap().files, previous);
    assert_eq!(state.lock().unwrap().writes, 1);
}

#[test]
fn storage_failures_preserve_previous_ciphertext_and_allow_retry() {
    let (mut store, state) = fixture();
    let id = SecretId::DonBotGw2ApiKey;
    store.store(id, &value()).unwrap();
    let previous = state.lock().unwrap().files.clone();
    for error in [
        FileError::DirectoryCreateFailed,
        FileError::WriteFailed,
        FileError::FlushFailed,
        FileError::ReplaceFailed,
    ] {
        state.lock().unwrap().write_error = Some(error);
        assert_eq!(
            store
                .store(id, &SecretValue::from_bytes(vec![42]))
                .unwrap_err()
                .code,
            SecretStoreErrorCode::File(error)
        );
        assert_eq!(state.lock().unwrap().files, previous);
    }
    state.lock().unwrap().read_error = Some(FileError::ChangedDuringRead);
    assert_eq!(
        code(store.load(id)),
        SecretStoreErrorCode::File(FileError::ChangedDuringRead)
    );
    state.lock().unwrap().erase_error = Some(FileError::DeleteFailed);
    assert_eq!(
        store.erase(id).unwrap_err().code,
        SecretStoreErrorCode::File(FileError::DeleteFailed)
    );
    assert_eq!(state.lock().unwrap().files, previous);
    {
        let mut state = state.lock().unwrap();
        state.write_error = None;
        state.read_error = None;
        state.erase_error = None;
    }
    store.store(id, &SecretValue::from_bytes(vec![42])).unwrap();
    assert_eq!(store.load(id).unwrap().expose(), &[42]);
    store.erase(id).unwrap();
    assert!(state.lock().unwrap().files.is_empty());
}
