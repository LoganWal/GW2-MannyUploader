use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use manny_uploader::application::{FileError, FileStore};
use manny_uploader::filesystem::{AtomicFileCommit, FilesystemStore, temporary_path};

struct Tree(PathBuf);
impl Tree {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "manny-storage-{}-{}-\u{96ea}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Tree {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[derive(Default)]
struct Calls {
    fail_replace: bool,
    replacements: usize,
    syncs: usize,
}
struct TestCommit(Arc<Mutex<Calls>>);
impl AtomicFileCommit for TestCommit {
    fn replace(&self, temporary: &Path, destination: &Path) -> Result<(), FileError> {
        let mut calls = self.0.lock().unwrap();
        calls.replacements += 1;
        if calls.fail_replace {
            return Err(FileError::ReplaceFailed);
        }
        fs::rename(temporary, destination).map_err(|_| FileError::ReplaceFailed)
    }
    fn sync_parent(&self, _: &Path) -> Result<(), FileError> {
        self.0.lock().unwrap().syncs += 1;
        Ok(())
    }
}

#[test]
fn unicode_atomic_writes_replace_and_erase_both_primary_and_stale_temporary() {
    let tree = Tree::new();
    let path = tree.0.join("nested/\u{96ea}.json");
    let calls = Arc::new(Mutex::new(Calls::default()));
    let store = FilesystemStore::new(TestCommit(calls.clone()));
    assert_eq!(store.read(&path, 64), Ok(None));
    store.write_atomic(&path, b"first").unwrap();
    assert_eq!(store.read(&path, 5).unwrap().unwrap(), b"first");
    assert!(!temporary_path(&path).exists());
    fs::write(temporary_path(&path), b"stale").unwrap();
    store.write_atomic(&path, b"second").unwrap();
    assert_eq!(store.read(&path, 6).unwrap().unwrap(), b"second");
    {
        let calls = calls.lock().unwrap();
        assert_eq!((calls.replacements, calls.syncs), (2, 2));
    }
    fs::write(temporary_path(&path), b"interrupted").unwrap();
    store.erase(&path).unwrap();
    store.erase(&path).unwrap();
    assert_eq!(store.read(&path, 64), Ok(None));
    assert!(!temporary_path(&path).exists());
    assert_eq!(calls.lock().unwrap().syncs, 3);
}

#[test]
fn failed_replacement_preserves_old_primary_and_later_write_recovers() {
    let tree = Tree::new();
    let path = tree.0.join("record");
    let calls = Arc::new(Mutex::new(Calls::default()));
    let store = FilesystemStore::new(TestCommit(calls.clone()));
    store.write_atomic(&path, b"old").unwrap();
    calls.lock().unwrap().fail_replace = true;
    assert_eq!(
        store.write_atomic(&path, b"replacement-marker"),
        Err(FileError::ReplaceFailed)
    );
    assert_eq!(fs::read(&path).unwrap(), b"old");
    assert_eq!(
        fs::read(temporary_path(&path)).unwrap(),
        b"replacement-marker"
    );
    assert_eq!(calls.lock().unwrap().syncs, 1);
    calls.lock().unwrap().fail_replace = false;
    store.write_atomic(&path, b"recovered").unwrap();
    assert_eq!(fs::read(&path).unwrap(), b"recovered");
    assert!(!temporary_path(&path).exists());
}

#[test]
fn read_bounds_and_invalid_paths_fail_before_mutation() {
    let tree = Tree::new();
    let path = tree.0.join("record");
    let store = FilesystemStore::new(TestCommit(Arc::new(Mutex::new(Calls::default()))));
    for invalid in [
        Path::new(""),
        Path::new("."),
        Path::new(".."),
        Path::new("file/"),
        Path::new("file/."),
        Path::new("nul\0suffix"),
    ] {
        assert_eq!(
            store.write_atomic(invalid, b"bytes"),
            Err(FileError::InvalidPath)
        );
        assert_eq!(store.erase(invalid), Err(FileError::InvalidPath));
    }
    store.write_atomic(&path, &[42; 8193]).unwrap();
    assert_eq!(store.read(&path, 0), Err(FileError::InvalidLimit));
    assert_eq!(store.read(&path, usize::MAX), Err(FileError::InvalidLimit));
    assert_eq!(store.read(&path, 8192), Err(FileError::TooLarge));
    assert_eq!(store.read(&path, 8193).unwrap().unwrap().len(), 8193);
    assert_eq!(store.read(&tree.0, 64), Err(FileError::ReadFailed));
    fs::create_dir(temporary_path(&path)).unwrap();
    assert_eq!(
        store.write_atomic(&path, b"replacement"),
        Err(FileError::DeleteFailed)
    );
    assert_eq!(fs::read(&path).unwrap(), vec![42; 8193]);
}

#[test]
fn stale_temporary_hardlink_is_removed_without_truncating_its_target() {
    let tree = Tree::new();
    let path = tree.0.join("record");
    let target = tree.0.join("other");
    fs::write(&target, b"preserve").unwrap();
    fs::hard_link(&target, temporary_path(&path)).unwrap();
    let store = FilesystemStore::new(TestCommit(Arc::new(Mutex::new(Calls::default()))));
    store.write_atomic(&path, b"new").unwrap();
    assert_eq!(fs::read(&target).unwrap(), b"preserve");
    assert_eq!(fs::read(&path).unwrap(), b"new");
}

#[test]
fn erase_attempts_temporary_cleanup_even_when_primary_cannot_be_deleted() {
    let tree = Tree::new();
    let path = tree.0.join("directory");
    fs::create_dir(&path).unwrap();
    fs::write(temporary_path(&path), b"stale").unwrap();
    let store = FilesystemStore::new(TestCommit(Arc::new(Mutex::new(Calls::default()))));
    assert_eq!(store.erase(&path), Err(FileError::DeleteFailed));
    assert!(!temporary_path(&path).exists());
    assert!(path.is_dir());
}

#[test]
fn oversized_writes_create_nothing_and_empty_files_remain_distinct_from_missing() {
    let tree = Tree::new();
    let path = tree.0.join("nested/record");
    let store = FilesystemStore::new(TestCommit(Arc::new(Mutex::new(Calls::default()))));
    assert_eq!(
        store.write_atomic(&path, &vec![0; 32 * 1024 * 1024 + 1]),
        Err(FileError::TooLarge)
    );
    assert!(!tree.0.join("nested").exists());
    store.write_atomic(&path, b"").unwrap();
    assert_eq!(store.read(&path, 1), Ok(Some(Vec::new())));
}

#[cfg(unix)]
#[test]
fn unix_commit_flushes_parent_and_does_not_follow_stale_temporary_symlinks() {
    use manny_uploader::filesystem::UnixFileCommit;
    use std::os::unix::fs::symlink;
    let tree = Tree::new();
    let path = tree.0.join("record");
    let other = tree.0.join("other");
    fs::write(&other, b"preserve").unwrap();
    symlink(&other, temporary_path(&path)).unwrap();
    let store = FilesystemStore::new(UnixFileCommit);
    store.write_atomic(&path, b"new").unwrap();
    assert_eq!(fs::read(other).unwrap(), b"preserve");
    store.write_atomic(&path, b"replacement").unwrap();
    store.erase(&path).unwrap();
}
