use std::panic::{AssertUnwindSafe, catch_unwind};

use crate::application::{
    CandidateBatch, CandidateError, CandidateIssue, CandidateSourceOptions, DirectoryChange,
    DirectoryChangeMonitor, LogCandidateSource, StopToken,
};

/// Notifications only decide when to scan. The authoritative source owns reconciliation.
pub struct NotifyingLogSource {
    source: Box<dyn LogCandidateSource>,
    monitor: Option<Box<dyn DirectoryChangeMonitor>>,
    cache: Option<CandidateBatch>,
    failures: usize,
    monitor_enabled: bool,
    pending_issue: Option<CandidateIssue>,
}

impl NotifyingLogSource {
    pub fn new(
        source: Box<dyn LogCandidateSource>,
        monitor: Option<Box<dyn DirectoryChangeMonitor>>,
        options: CandidateSourceOptions,
    ) -> Result<Self, CandidateError> {
        let mut result = Self {
            source,
            monitor,
            cache: None,
            failures: 0,
            monitor_enabled: false,
            pending_issue: None,
        };
        result.reconfigure(options)?;
        Ok(result)
    }

    pub fn using_notifications(&self) -> bool {
        self.monitor_enabled
    }

    pub fn consecutive_failures(&self) -> usize {
        self.failures
    }
}

impl LogCandidateSource for NotifyingLogSource {
    fn reconfigure(&mut self, options: CandidateSourceOptions) -> Result<(), CandidateError> {
        self.source.reconfigure(options.clone())?;
        self.cache = None;
        self.failures = 0;
        self.pending_issue = None;
        self.monitor_enabled = false;
        if let Some(monitor) = &mut self.monitor {
            self.monitor_enabled = catch_unwind(AssertUnwindSafe(|| {
                monitor.reconfigure(&options.root, options.recursive)
            }))
            .unwrap_or(Err(CandidateError::Panicked))
            .is_ok();
            if !self.monitor_enabled {
                self.pending_issue = Some(CandidateIssue::NotificationsDisabled);
            }
        }
        Ok(())
    }

    fn poll(&mut self, stop: &StopToken) -> Result<CandidateBatch, CandidateError> {
        if stop.is_stop_requested() {
            return Err(CandidateError::Cancelled);
        }
        let mut issue = self.pending_issue.take();
        if self.monitor_enabled {
            let change = catch_unwind(AssertUnwindSafe(|| {
                self.monitor.as_mut().unwrap().poll(stop)
            }))
            .unwrap_or(Err(CandidateError::Panicked));
            match change {
                Ok(status) => {
                    self.failures = 0;
                    if status == DirectoryChange::Unchanged {
                        if let Some(cache) = &self.cache {
                            return Ok(cache.clone());
                        }
                    }
                }
                Err(CandidateError::Cancelled) => return Err(CandidateError::Cancelled),
                Err(_) => {
                    self.failures += 1;
                    self.monitor_enabled = self.failures < 3;
                    issue = Some(if self.monitor_enabled {
                        CandidateIssue::NotificationFailed
                    } else {
                        CandidateIssue::NotificationsDisabled
                    });
                }
            }
        }
        // A failed authoritative scan must never leave an older successful cache reusable.
        self.cache = None;
        let mut batch = catch_unwind(AssertUnwindSafe(|| self.source.poll(stop)))
            .unwrap_or(Err(CandidateError::Panicked))?;
        if batch.root_available && batch.scan_complete {
            let mut cache = batch.clone();
            cache.removed_paths.clear();
            cache.issues.clear();
            self.cache = Some(cache);
        }
        if let Some(issue) = issue {
            batch.issues.push(issue);
            batch.issues.sort_unstable();
        }
        Ok(batch)
    }
}
