use std::path::PathBuf;
use std::time::SystemTime;

use super::StopToken;
use crate::domain::LogFileIdentity;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CandidateSourceOptions {
    pub root: PathBuf,
    pub recursive: bool,
    pub max_candidates: usize,
    pub minimum_last_write_time: Option<SystemTime>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CandidateError {
    InvalidConfiguration,
    Cancelled,
    ScanFailed,
    ResourceLimit,
    Panicked,
}

/// Safe status categories. Filesystem errors and local paths never enter diagnostics.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum CandidateIssue {
    EntryUnavailable,
    DirectoryUnavailable,
    TimestampUnavailable,
    CanonicalPathUnavailable,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CandidateBatch {
    pub root_available: bool,
    pub scan_complete: bool,
    pub observations: Vec<LogFileIdentity>,
    pub removed_paths: Vec<PathBuf>,
    pub issues: Vec<CandidateIssue>,
}

/// Called only by the application owner, never from a render callback.
pub trait LogCandidateSource: Send {
    fn poll(&mut self, stop: &StopToken) -> Result<CandidateBatch, CandidateError>;
    /// Failure preserves configuration and retained observations.
    fn reconfigure(&mut self, options: CandidateSourceOptions) -> Result<(), CandidateError>;
}
