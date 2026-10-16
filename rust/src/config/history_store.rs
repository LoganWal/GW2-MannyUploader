use std::collections::HashMap;
use std::path::{Path, PathBuf};

use super::history_json::{validate_capacity, validate_records};
use super::{
    HistoryDocumentError, MAX_HISTORY_BYTES, MAX_HISTORY_RECORDS, decode_history, encode_history,
};
use crate::application::{
    FileStore, HistoryRecovery, HistoryStoreError, UploadHistoryStore, validate_storage_path,
};
use crate::domain::UploadJobRecord;

pub struct JsonUploadHistoryStore {
    files: Box<dyn FileStore>,
    path: PathBuf,
    capacity: usize,
    records: Vec<UploadJobRecord>,
    recovery: Option<HistoryRecovery>,
}

impl JsonUploadHistoryStore {
    pub fn new(
        files: Box<dyn FileStore>,
        path: PathBuf,
        capacity: usize,
    ) -> Result<Self, HistoryStoreError> {
        validate_storage_path(&path).map_err(|_| HistoryStoreError::InvalidConfiguration)?;
        validate_capacity(capacity).map_err(map_error)?;
        let (records, recovery) = match files.read(&path, MAX_HISTORY_BYTES) {
            Ok(None) => (Vec::new(), None),
            Ok(Some(bytes)) => match decode_history(&bytes, capacity) {
                Ok(records) => (records, None),
                Err(_) => (Vec::new(), Some(HistoryRecovery::InvalidDocument)),
            },
            Err(error) => (Vec::new(), Some(HistoryRecovery::File(error))),
        };
        Ok(Self {
            files,
            path,
            capacity,
            records,
            recovery,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl UploadHistoryStore for JsonUploadHistoryStore {
    fn records(&self) -> &[UploadJobRecord] {
        &self.records
    }
    fn recovery(&self) -> Option<HistoryRecovery> {
        self.recovery
    }

    fn merge_and_save(&mut self, updates: &[UploadJobRecord]) -> Result<(), HistoryStoreError> {
        // Validate even records that capacity trimming would discard before cloning caller data.
        validate_records(updates, MAX_HISTORY_RECORDS).map_err(map_error)?;
        let mut indices: HashMap<_, _> = self
            .records
            .iter()
            .enumerate()
            .map(|(index, record)| (&record.file, index))
            .collect();
        let mut merged = self.records.clone();
        for record in updates {
            if let Some(&index) = indices.get(&record.file) {
                merged[index] = record.clone();
            } else {
                indices.insert(&record.file, merged.len());
                merged.push(record.clone());
            }
        }
        let excess = merged.len().saturating_sub(self.capacity);
        merged.drain(..excess);
        let bytes = encode_history(&merged, self.capacity).map_err(map_error)?;
        self.files
            .write_atomic(&self.path, &bytes)
            .map_err(HistoryStoreError::File)?;
        self.records = merged;
        self.recovery = None;
        Ok(())
    }
}

fn map_error(error: HistoryDocumentError) -> HistoryStoreError {
    match error {
        HistoryDocumentError::InvalidCapacity => HistoryStoreError::InvalidConfiguration,
        HistoryDocumentError::TooLarge => HistoryStoreError::TooLarge,
        HistoryDocumentError::DuplicateIdentity => HistoryStoreError::DuplicateIdentity,
        HistoryDocumentError::SerializationFailed => HistoryStoreError::SerializationFailed,
        HistoryDocumentError::InvalidJson
        | HistoryDocumentError::InvalidSchema
        | HistoryDocumentError::InvalidRecord => HistoryStoreError::InvalidRecord,
    }
}
