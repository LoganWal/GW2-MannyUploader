use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Read, Write};
use std::path::{Path, PathBuf};

use crate::application::{FileError, FileStore, validate_storage_path as validate_path};

const MAX_PERSISTENT_BYTES: usize = 32 * 1024 * 1024;

/// Platform commit seam. Windows FFI stays in the native crate, outside the safe portable core.
pub trait AtomicFileCommit: Send {
    fn replace(&self, temporary: &Path, destination: &Path) -> Result<(), FileError>;
    fn sync_parent(&self, path: &Path) -> Result<(), FileError>;
}

pub struct FilesystemStore<C: AtomicFileCommit> {
    commit: C,
}
impl<C: AtomicFileCommit> FilesystemStore<C> {
    pub fn new(commit: C) -> Self {
        Self { commit }
    }
}

pub fn temporary_path(path: &Path) -> PathBuf {
    let mut temporary = path.as_os_str().to_os_string();
    temporary.push(".tmp");
    temporary.into()
}

impl<C: AtomicFileCommit> FileStore for FilesystemStore<C> {
    fn read(&self, path: &Path, max_bytes: usize) -> Result<Option<Vec<u8>>, FileError> {
        validate_path(path)?;
        if !(1..=MAX_PERSISTENT_BYTES).contains(&max_bytes) {
            return Err(FileError::InvalidLimit);
        }
        let status = match fs::symlink_metadata(path) {
            Ok(status) => status,
            Err(error) if error.kind() == ErrorKind::NotFound => return Ok(None),
            Err(_) => return Err(FileError::ReadFailed),
        };
        if !status.is_file() || status.file_type().is_symlink() {
            return Err(FileError::ReadFailed);
        }
        let mut file = File::open(path).map_err(|_| FileError::ReadFailed)?;
        let before = file.metadata().map_err(|_| FileError::ReadFailed)?;
        if !before.is_file() {
            return Err(FileError::ReadFailed);
        }
        if before.len() > max_bytes as u64 {
            return Err(FileError::TooLarge);
        }
        let mut contents = Vec::new();
        contents
            .try_reserve_exact(before.len() as usize)
            .map_err(|_| FileError::AllocationFailed)?;
        let mut block = [0; 8192];
        loop {
            let count = file.read(&mut block).map_err(|_| FileError::ReadFailed)?;
            if count == 0 {
                break;
            }
            if count > max_bytes - contents.len() {
                return Err(FileError::TooLarge);
            }
            contents
                .try_reserve(count)
                .map_err(|_| FileError::AllocationFailed)?;
            contents.extend_from_slice(&block[..count]);
        }
        let after = file.metadata().map_err(|_| FileError::ReadFailed)?;
        if before.len() != contents.len() as u64
            || after.len() != before.len()
            || before.modified().map_err(|_| FileError::ReadFailed)?
                != after.modified().map_err(|_| FileError::ReadFailed)?
        {
            return Err(FileError::ChangedDuringRead);
        }
        Ok(Some(contents))
    }

    fn write_atomic(&self, path: &Path, contents: &[u8]) -> Result<(), FileError> {
        validate_path(path)?;
        if contents.len() > MAX_PERSISTENT_BYTES {
            return Err(FileError::TooLarge);
        }
        if let Some(parent) = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
        {
            fs::create_dir_all(parent).map_err(|_| FileError::DirectoryCreateFailed)?;
        }
        let temporary = temporary_path(path);
        // Single-owner storage can discard a stale temporary. Exclusive creation prevents following
        // a pre-existing temporary symlink or truncating a hard-linked file.
        remove_if_present(&temporary)?;
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|_| FileError::WriteFailed)?;
        file.write_all(contents)
            .map_err(|_| FileError::WriteFailed)?;
        file.sync_all().map_err(|_| FileError::FlushFailed)?;
        drop(file);
        self.commit.replace(&temporary, path)?;
        self.commit.sync_parent(path)
    }

    fn erase(&self, path: &Path) -> Result<(), FileError> {
        validate_path(path)?;
        // Attempt both removals even if one fails, so a stale protected record is not left untried.
        let primary = remove_if_present(path);
        let temporary = remove_if_present(&temporary_path(path));
        let changed = matches!(primary, Ok(true)) || matches!(temporary, Ok(true));
        let synced = if changed {
            self.commit.sync_parent(path)
        } else {
            Ok(())
        };
        primary?;
        temporary?;
        synced
    }
}

fn remove_if_present(path: &Path) -> Result<bool, FileError> {
    match fs::remove_file(path) {
        Ok(()) => Ok(true),
        Err(error) if error.kind() == ErrorKind::NotFound => Ok(false),
        Err(_) => Err(FileError::DeleteFailed),
    }
}

#[cfg(unix)]
pub struct UnixFileCommit;
#[cfg(unix)]
impl AtomicFileCommit for UnixFileCommit {
    fn replace(&self, temporary: &Path, destination: &Path) -> Result<(), FileError> {
        fs::rename(temporary, destination).map_err(|_| FileError::ReplaceFailed)
    }
    fn sync_parent(&self, path: &Path) -> Result<(), FileError> {
        let parent = path
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        File::open(parent)
            .and_then(|directory| directory.sync_all())
            .map_err(|_| FileError::FlushFailed)
    }
}
