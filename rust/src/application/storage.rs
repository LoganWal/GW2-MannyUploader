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
