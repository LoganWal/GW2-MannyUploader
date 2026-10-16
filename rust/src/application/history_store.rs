use crate::domain::UploadJobRecord;

use super::FileError;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HistoryStoreError {
    InvalidConfiguration,
    InvalidRecord,
    TooLarge,
    DuplicateIdentity,
    SerializationFailed,
    File(FileError),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HistoryRecovery {
    InvalidDocument,
    File(FileError),
}

/// Application-owner persistence. Render callbacks must never call this port.
pub trait UploadHistoryStore: Send {
    fn records(&self) -> &[UploadJobRecord];
    fn recovery(&self) -> Option<HistoryRecovery>;
    /// Preserve retained rows outside the UI window and commit memory only after a durable write.
    fn merge_and_save(&mut self, updates: &[UploadJobRecord]) -> Result<(), HistoryStoreError>;
}
