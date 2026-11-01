use std::collections::{HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};

use super::LogFileIdentity;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiscoveryError {
    InvalidConfiguration,
    InvalidCandidate,
    ResourceLimit,
}

pub fn is_zevtc_candidate(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("zevtc"))
}

/// Pure observation policy. The caller supplies canonical paths and schedules observations.
pub struct LogDiscovery {
    required_matches: usize,
    pending_limit: usize,
    history_limit: usize,
    pending: HashMap<PathBuf, (LogFileIdentity, usize)>,
    accepted: VecDeque<LogFileIdentity>,
    known: HashSet<LogFileIdentity>,
}

impl LogDiscovery {
    pub fn new(
        required_matches: usize,
        pending_limit: usize,
        history_limit: usize,
    ) -> Result<Self, DiscoveryError> {
        if required_matches < 2
            || !(1..=10_000).contains(&pending_limit)
            || !(1..=10_000).contains(&history_limit)
        {
            return Err(DiscoveryError::InvalidConfiguration);
        }
        Ok(Self {
            required_matches,
            pending_limit,
            history_limit,
            pending: HashMap::new(),
            accepted: VecDeque::new(),
            known: HashSet::new(),
        })
    }

    pub fn observe(
        &mut self,
        file: LogFileIdentity,
    ) -> Result<Option<LogFileIdentity>, DiscoveryError> {
        Self::validate(&file)?;
        if self.known.contains(&file) {
            self.pending.remove(&file.canonical_path);
            return Ok(None);
        }
        let matches = match self.pending.get(&file.canonical_path) {
            Some((previous, count)) if previous == &file => count.saturating_add(1),
            Some(_) => 1,
            None => {
                if self.pending.len() == self.pending_limit {
                    return Err(DiscoveryError::ResourceLimit);
                }
                self.pending
                    .try_reserve(1)
                    .map_err(|_| DiscoveryError::ResourceLimit)?;
                1
            }
        };
        if matches < self.required_matches {
            self.pending
                .insert(file.canonical_path.clone(), (file, matches));
            return Ok(None);
        }
        self.remember(file.clone())?;
        self.pending.remove(&file.canonical_path);
        Ok(Some(file))
    }

    fn validate(file: &LogFileIdentity) -> Result<(), DiscoveryError> {
        if file.canonical_path.as_os_str().is_empty() || !is_zevtc_candidate(&file.canonical_path) {
            return Err(DiscoveryError::InvalidCandidate);
        }
        Ok(())
    }

    fn remember(&mut self, file: LogFileIdentity) -> Result<(), DiscoveryError> {
        if self.known.contains(&file) {
            return Ok(());
        }
        self.accepted
            .try_reserve(1)
            .map_err(|_| DiscoveryError::ResourceLimit)?;
        self.known
            .try_reserve(1)
            .map_err(|_| DiscoveryError::ResourceLimit)?;
        if self.accepted.len() == self.history_limit {
            if let Some(oldest) = self.accepted.pop_front() {
                self.known.remove(&oldest);
            }
        }
        self.known.insert(file.clone());
        self.accepted.push_back(file);
        Ok(())
    }

    /// Validate the complete retained history before replacing any dedupe state.
    pub fn seed(&mut self, files: &[LogFileIdentity]) -> Result<(), DiscoveryError> {
        if files.len() > self.history_limit {
            return Err(DiscoveryError::ResourceLimit);
        }
        for file in files {
            Self::validate(file)?;
        }
        self.accepted
            .try_reserve(files.len())
            .map_err(|_| DiscoveryError::ResourceLimit)?;
        self.known
            .try_reserve(files.len())
            .map_err(|_| DiscoveryError::ResourceLimit)?;
        for file in files {
            self.remember(file.clone())?;
        }
        Ok(())
    }

    pub fn release(&mut self, file: &LogFileIdentity) {
        self.accepted.retain(|accepted| accepted != file);
        self.known.remove(file);
    }

    pub fn forget_pending(&mut self, path: &Path) {
        self.pending.remove(path);
    }

    pub fn clear_pending(&mut self) {
        self.pending.clear();
    }

    pub fn set_required_matches(&mut self, required_matches: usize) -> Result<(), DiscoveryError> {
        if required_matches < 2 {
            return Err(DiscoveryError::InvalidConfiguration);
        }
        self.required_matches = required_matches;
        self.clear_pending();
        Ok(())
    }

    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    pub fn accepted_count(&self) -> usize {
        self.accepted.len()
    }
}
