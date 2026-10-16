use std::collections::BTreeSet;
use std::fs::{self, Metadata};
use std::io::ErrorKind;
use std::path::PathBuf;

use crate::application::{
    CandidateBatch, CandidateError, CandidateIssue, CandidateSourceOptions, LogCandidateSource,
    StopToken,
};
use crate::domain::{LogFileIdentity, is_zevtc_candidate};

const MAX_DEPTH: usize = 128;
const MAX_ENTRIES: usize = 1_000_000;

#[derive(Default)]
struct SnapshotTracker {
    retained: BTreeSet<PathBuf>,
}

impl SnapshotTracker {
    fn reconcile(
        &mut self,
        mut batch: CandidateBatch,
        limit: usize,
    ) -> Result<CandidateBatch, CandidateError> {
        batch
            .observations
            .sort_by(|a, b| a.canonical_path.cmp(&b.canonical_path));
        batch.issues.sort_unstable();
        let seen: BTreeSet<_> = batch
            .observations
            .iter()
            .map(|file| file.canonical_path.clone())
            .collect();
        if !batch.root_available || batch.scan_complete {
            batch.removed_paths = self.retained.difference(&seen).cloned().collect();
            self.retained = seen;
        } else {
            let additional = seen.difference(&self.retained).count();
            if self.retained.len().saturating_add(additional) > limit {
                return Err(CandidateError::ResourceLimit);
            }
            self.retained.extend(seen);
        }
        Ok(batch)
    }
}

pub struct PollingLogSource {
    options: CandidateSourceOptions,
    tracker: SnapshotTracker,
}

impl PollingLogSource {
    pub fn new(mut options: CandidateSourceOptions) -> Result<Self, CandidateError> {
        if options.root.as_os_str().is_empty() || !(1..=10_000).contains(&options.max_candidates) {
            return Err(CandidateError::InvalidConfiguration);
        }
        options.root =
            std::path::absolute(&options.root).map_err(|_| CandidateError::InvalidConfiguration)?;
        Ok(Self {
            options,
            tracker: SnapshotTracker::default(),
        })
    }

    pub fn options(&self) -> &CandidateSourceOptions {
        &self.options
    }

    pub fn retained_count(&self) -> usize {
        self.tracker.retained.len()
    }

    fn scan(&self, stop: &StopToken) -> Result<CandidateBatch, CandidateError> {
        check_stop(stop)?;
        let mut batch = CandidateBatch {
            scan_complete: true,
            ..Default::default()
        };
        let root = match fs::symlink_metadata(&self.options.root) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == ErrorKind::NotFound => return Ok(batch),
            Err(_) => return Err(CandidateError::ScanFailed),
        };
        if !root.is_dir() || is_link(&root) {
            return Err(CandidateError::ScanFailed);
        }
        batch.root_available = true;
        let mut stack =
            vec![fs::read_dir(&self.options.root).map_err(|_| CandidateError::ScanFailed)?];
        let mut entries = 0_usize;
        let mut eligible = 0_usize;
        // Keep one iterator per depth, rather than collecting an unbounded directory listing.
        while let Some(directory) = stack.last_mut() {
            check_stop(stop)?;
            entries += 1;
            if entries > MAX_ENTRIES {
                return Err(CandidateError::ResourceLimit);
            }
            let entry = match directory.next() {
                None => {
                    stack.pop();
                    continue;
                }
                Some(Err(_)) => {
                    add_issue(&mut batch, CandidateIssue::EntryUnavailable);
                    continue;
                }
                Some(Ok(entry)) => entry,
            };
            let path = entry.path();
            let metadata = match fs::symlink_metadata(&path) {
                Ok(metadata) => metadata,
                Err(_) => {
                    add_issue(&mut batch, CandidateIssue::EntryUnavailable);
                    continue;
                }
            };
            if is_link(&metadata) {
                continue;
            }
            if metadata.is_dir() && self.options.recursive {
                if stack.len() == MAX_DEPTH {
                    return Err(CandidateError::ResourceLimit);
                }
                match fs::read_dir(&path) {
                    Ok(child) => stack.push(child),
                    Err(_) => add_issue(&mut batch, CandidateIssue::DirectoryUnavailable),
                }
                continue;
            }
            if !metadata.is_file() || !is_zevtc_candidate(&path) {
                continue;
            }
            let last_write_time = match metadata.modified() {
                Ok(time) => time,
                Err(_) => {
                    add_issue(&mut batch, CandidateIssue::TimestampUnavailable);
                    continue;
                }
            };
            if self
                .options
                .minimum_last_write_time
                .is_some_and(|cutoff| last_write_time < cutoff)
            {
                continue;
            }
            eligible += 1;
            if eligible > self.options.max_candidates {
                return Err(CandidateError::ResourceLimit);
            }
            let canonical_path = match fs::canonicalize(path) {
                Ok(path) => path,
                Err(_) => {
                    add_issue(&mut batch, CandidateIssue::CanonicalPathUnavailable);
                    continue;
                }
            };
            batch.observations.push(LogFileIdentity {
                canonical_path,
                size: metadata.len(),
                last_write_time,
            });
        }
        check_stop(stop)?;
        Ok(batch)
    }
}

impl LogCandidateSource for PollingLogSource {
    fn poll(&mut self, stop: &StopToken) -> Result<CandidateBatch, CandidateError> {
        let batch = self.scan(stop)?;
        self.tracker.reconcile(batch, self.options.max_candidates)
    }

    fn reconfigure(&mut self, options: CandidateSourceOptions) -> Result<(), CandidateError> {
        *self = Self::new(options)?;
        Ok(())
    }
}

fn check_stop(stop: &StopToken) -> Result<(), CandidateError> {
    if stop.is_stop_requested() {
        Err(CandidateError::Cancelled)
    } else {
        Ok(())
    }
}

fn add_issue(batch: &mut CandidateBatch, issue: CandidateIssue) {
    batch.scan_complete = false;
    // One diagnostic per category bounds memory even in an inaccessible tree.
    if !batch.issues.contains(&issue) {
        batch.issues.push(issue);
    }
}

fn is_link(metadata: &Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        // Reject all reparse points, including junctions that can escape the configured tree.
        metadata.file_attributes() & 0x400 != 0
    }
    #[cfg(not(windows))]
    {
        metadata.file_type().is_symlink()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::SystemTime;

    fn snapshot(paths: &[&str], complete: bool) -> CandidateBatch {
        CandidateBatch {
            root_available: true,
            scan_complete: complete,
            observations: paths
                .iter()
                .map(|path| LogFileIdentity {
                    canonical_path: path.into(),
                    size: 1,
                    last_write_time: SystemTime::UNIX_EPOCH,
                })
                .collect(),
            ..Default::default()
        }
    }

    #[test]
    fn incomplete_scans_preserve_paths_until_a_complete_scan_and_bound_retention() {
        let mut tracker = SnapshotTracker::default();
        tracker.reconcile(snapshot(&["b", "a"], true), 3).unwrap();
        assert!(
            tracker
                .reconcile(snapshot(&["c"], false), 3)
                .unwrap()
                .removed_paths
                .is_empty()
        );
        assert_eq!(
            tracker.reconcile(snapshot(&["d"], false), 3),
            Err(CandidateError::ResourceLimit)
        );
        assert_eq!(tracker.retained.len(), 3);
        assert_eq!(
            tracker
                .reconcile(snapshot(&["b"], true), 3)
                .unwrap()
                .removed_paths,
            vec![PathBuf::from("a"), PathBuf::from("c")]
        );
        assert_eq!(
            tracker
                .reconcile(CandidateBatch::default(), 3)
                .unwrap()
                .removed_paths,
            vec![PathBuf::from("b")]
        );
        assert!(tracker.retained.is_empty());
    }
}
