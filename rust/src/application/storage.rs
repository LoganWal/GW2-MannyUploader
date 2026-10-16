use std::path::Path;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileError {
    InvalidPath,
    InvalidLimit,
    TooLarge,
    AllocationFailed,
    ReadFailed,
    ChangedDuringRead,
    DirectoryCreateFailed,
    WriteFailed,
    FlushFailed,
    ReplaceFailed,
    DeleteFailed,
}

/// One application owner uses persistent storage outside callbacks. Credentials must be protected
/// before their bytes reach this port. Writers must not concurrently target the same path.
pub trait FileStore: Send {
    fn read(&self, path: &Path, max_bytes: usize) -> Result<Option<Vec<u8>>, FileError>;
    fn write_atomic(&self, path: &Path, contents: &[u8]) -> Result<(), FileError>;
    /// Idempotently removes the primary and its interrupted-write temporary file.
    fn erase(&self, path: &Path) -> Result<(), FileError>;
}

pub(crate) fn validate_storage_path(path: &Path) -> Result<(), FileError> {
    let bytes = path.as_os_str().as_encoded_bytes();
    let last = bytes
        .rsplit(|byte| *byte == b'/' || (cfg!(windows) && *byte == b'\\'))
        .next()
        .unwrap_or_default();
    if path.file_name().is_none()
        || last.is_empty()
        || last == b"."
        || last == b".."
        || bytes.contains(&0)
    {
        Err(FileError::InvalidPath)
    } else {
        Ok(())
    }
}
