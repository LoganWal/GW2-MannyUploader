use super::{Clock, CoordinatorError, UploadCoordinator, UploadJobSnapshot};
use crate::domain::{Provider, UploadJobId, WVW_BOSS_ID};
use std::{
    collections::VecDeque,
    panic::{AssertUnwindSafe, catch_unwind},
    path::Path,
    sync::{Mutex, MutexGuard},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExternalActionError {
    InvalidTarget,
    LaunchFailed,
}
pub trait ExternalActionLauncher: Send {
    fn open_url(&mut self, url: &str) -> Result<(), ExternalActionError>;
    fn open_directory(&mut self, directory: &Path) -> Result<(), ExternalActionError>;
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReportTarget {
    DpsReport,
    Wingman,
    DonBot,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecentLogAction {
    OpenReport {
        job_id: UploadJobId,
        target: ReportTarget,
    },
    OpenDirectory(UploadJobId),
    Retry {
        job_id: UploadJobId,
        provider: Provider,
    },
    Reupload(UploadJobId),
    Rechat(UploadJobId),
    DismissError,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecentLogActionError {
    InvalidConfiguration,
    AllocationFailed,
    QueueFull,
    UnknownJob,
    ReportUnavailable,
    UnsafeReportUrl,
    InvalidDirectory,
    LaunchFailed(ExternalActionError),
    RetryFailed(CoordinatorError),
    Busy,
    ShuttingDown,
    RevisionExhausted,
    Internal,
}
impl RecentLogActionError {
    pub fn detail(self) -> &'static str {
        match self {
            Self::InvalidConfiguration => "Recent-log actions are configured incorrectly",
            Self::AllocationFailed => "The recent-log command queue could not be created",
            Self::QueueFull => "The recent-log command queue is full",
            Self::UnknownJob => "The selected log is no longer retained",
            Self::ReportUnavailable => "The selected log has no report for this destination",
            Self::UnsafeReportUrl => "The selected report link is invalid",
            Self::InvalidDirectory => "The selected log directory is unavailable",
            Self::LaunchFailed(_) => "The selected report or directory could not be opened",
            Self::RetryFailed(_) => "The selected log action could not be started",
            Self::Busy => "Recent-log actions are already being processed",
            Self::ShuttingDown => "Recent-log actions are shutting down",
            Self::RevisionExhausted => "Recent-log actions cannot accept more commands",
            Self::Internal => "The recent-log action could not be completed",
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecentLogActionsOptions {
    pub command_capacity: usize,
    pub max_commands_per_tick: usize,
}
impl Default for RecentLogActionsOptions {
    fn default() -> Self {
        Self {
            command_capacity: 32,
            max_commands_per_tick: 8,
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecentLogActionsSnapshot {
    pub last_error: Option<RecentLogActionError>,
    pub pending_commands: usize,
    pub revision: u64,
    pub accepting_commands: bool,
    pub shutting_down: bool,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RecentLogActionsTickReport {
    pub commands_processed: usize,
    pub action_failures: usize,
}
struct Queue {
    commands: VecDeque<RecentLogAction>,
    snapshot: RecentLogActionsSnapshot,
    ticking: bool,
}
pub struct RecentLogActionsController {
    options: RecentLogActionsOptions,
    queue: Mutex<Queue>,
}
struct TickGuard<'a>(&'a RecentLogActionsController);
impl Drop for TickGuard<'_> {
    fn drop(&mut self) {
        self.0.lock().ticking = false;
    }
}
impl RecentLogActionsController {
    pub fn new(options: RecentLogActionsOptions) -> Result<Self, RecentLogActionError> {
        if options.command_capacity == 0 || options.max_commands_per_tick == 0 {
            return Err(RecentLogActionError::InvalidConfiguration);
        }
        let mut commands = VecDeque::new();
        commands
            .try_reserve_exact(options.command_capacity)
            .map_err(|_| RecentLogActionError::AllocationFailed)?;
        Ok(Self {
            options,
            queue: Mutex::new(Queue {
                commands,
                snapshot: RecentLogActionsSnapshot {
                    last_error: None,
                    pending_commands: 0,
                    revision: 1,
                    accepting_commands: true,
                    shutting_down: false,
                },
                ticking: false,
            }),
        })
    }
    fn lock(&self) -> MutexGuard<'_, Queue> {
        self.queue
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }
    pub fn snapshot(&self) -> RecentLogActionsSnapshot {
        self.lock().snapshot.clone()
    }
    pub fn submit(&self, command: RecentLogAction) -> Result<(), RecentLogActionError> {
        let mut queue = self.lock();
        if queue.snapshot.shutting_down {
            return Err(RecentLogActionError::ShuttingDown);
        }
        if queue.snapshot.revision >= u64::MAX - 1 {
            return Err(RecentLogActionError::RevisionExhausted);
        }
        if queue.commands.len() == self.options.command_capacity {
            queue.snapshot.last_error = Some(RecentLogActionError::QueueFull);
            queue.snapshot.revision += 1;
            return Err(RecentLogActionError::QueueFull);
        }
        queue.commands.push_back(command);
        queue.snapshot.pending_commands = queue.commands.len();
        queue.snapshot.revision += 1;
        Ok(())
    }
    /// Only the application owner calls tick. No queue lock crosses a launcher or coordinator call.
    pub fn tick<C: Clock>(
        &self,
        uploads: &mut UploadCoordinator<C>,
        launcher: &mut dyn ExternalActionLauncher,
    ) -> Result<RecentLogActionsTickReport, RecentLogActionError> {
        let budget = {
            let mut queue = self.lock();
            if queue.snapshot.shutting_down {
                return Err(RecentLogActionError::ShuttingDown);
            }
            if queue.ticking {
                return Err(RecentLogActionError::Busy);
            }
            queue.ticking = true;
            queue.commands.len().min(self.options.max_commands_per_tick)
        };
        let _ticking = TickGuard(self);
        let mut report = RecentLogActionsTickReport::default();
        for _ in 0..budget {
            let command = {
                let mut queue = self.lock();
                if queue.snapshot.shutting_down {
                    break;
                }
                let Some(command) = queue.commands.pop_front() else {
                    break;
                };
                queue.snapshot.pending_commands = queue.commands.len();
                queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
                command
            };
            let result = catch_unwind(AssertUnwindSafe(|| execute(command, uploads, launcher)))
                .unwrap_or(Err(RecentLogActionError::Internal));
            report.commands_processed += 1;
            if result.is_err() {
                report.action_failures += 1;
            }
            let mut queue = self.lock();
            if !queue.snapshot.shutting_down {
                queue.snapshot.last_error = result.err();
                queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
            }
        }
        Ok(report)
    }
    pub fn shutdown(&self) {
        let mut queue = self.lock();
        if queue.snapshot.shutting_down {
            return;
        }
        queue.commands.clear();
        queue.snapshot.pending_commands = 0;
        queue.snapshot.accepting_commands = false;
        queue.snapshot.shutting_down = true;
        queue.snapshot.revision = queue.snapshot.revision.saturating_add(1);
    }
}
impl Drop for RecentLogActionsController {
    fn drop(&mut self) {
        self.shutdown();
    }
}
fn execute<C: Clock>(
    command: RecentLogAction,
    uploads: &mut UploadCoordinator<C>,
    launcher: &mut dyn ExternalActionLauncher,
) -> Result<(), RecentLogActionError> {
    match command {
        RecentLogAction::DismissError => Ok(()),
        RecentLogAction::OpenReport { job_id, target } => {
            let snapshot = uploads
                .snapshot(job_id)
                .map_err(|_| RecentLogActionError::UnknownJob)?;
            let url = report_url(&snapshot, target)?;
            launcher
                .open_url(&url)
                .map_err(RecentLogActionError::LaunchFailed)
        }
        RecentLogAction::OpenDirectory(id) => {
            let snapshot = uploads
                .snapshot(id)
                .map_err(|_| RecentLogActionError::UnknownJob)?;
            let directory = snapshot
                .record
                .file
                .canonical_path
                .parent()
                .filter(|path| !path.as_os_str().is_empty())
                .ok_or(RecentLogActionError::InvalidDirectory)?;
            launcher
                .open_directory(directory)
                .map_err(RecentLogActionError::LaunchFailed)
        }
        RecentLogAction::Retry { job_id, provider } => {
            uploads.retry_failed(job_id, provider).map_err(action_error)
        }
        RecentLogAction::Reupload(id) => uploads.reupload(id).map_err(action_error),
        RecentLogAction::Rechat(id) => uploads.rechat(id).map_err(action_error),
    }
}
fn action_error(error: CoordinatorError) -> RecentLogActionError {
    if error == CoordinatorError::UnknownJob {
        RecentLogActionError::UnknownJob
    } else {
        RecentLogActionError::RetryFailed(error)
    }
}

/// Shared target resolution for actions and immutable presentation. No caller-supplied URL enters
/// the command queue, and errors contain neither target text nor local paths.
pub fn report_url(
    job: &UploadJobSnapshot,
    target: ReportTarget,
) -> Result<String, RecentLogActionError> {
    let (url, prefixes): (&str, &[&str]) = match target {
        ReportTarget::DpsReport => {
            let report = job
                .record
                .dps_report_result
                .as_ref()
                .ok_or(RecentLogActionError::ReportUnavailable)?;
            let prefixes: &[&str] = if report.boss_id == WVW_BOSS_ID {
                &[
                    "https://dps.report/",
                    "https://b.dps.report/",
                    "https://wvw.report/",
                ]
            } else {
                &["https://dps.report/", "https://b.dps.report/"]
            };
            (&report.permalink, prefixes)
        }
        ReportTarget::Wingman => (
            &job.record
                .wingman_upload_receipt
                .as_ref()
                .ok_or(RecentLogActionError::ReportUnavailable)?
                .permalink,
            &["https://gw2wingman.nevermindcreations.de/log/"],
        ),
        ReportTarget::DonBot => {
            let id = job
                .record
                .donbot_upload_receipt
                .as_ref()
                .and_then(|receipt| receipt.fight_log_id)
                .filter(|&id| valid_donbot_fight_id(id))
                .ok_or(RecentLogActionError::ReportUnavailable)?;
            return Ok(format!("https://donbot.walmslo.com/logs/{id}"));
        }
    };
    if !valid_external_url(url)
        || !prefixes.iter().any(|prefix| {
            url.strip_prefix(prefix)
                .is_some_and(|suffix| !suffix.is_empty())
        })
    {
        return Err(RecentLogActionError::UnsafeReportUrl);
    }
    Ok(url.to_owned())
}
pub fn valid_donbot_fight_id(id: u64) -> bool {
    id > 0 && id <= i64::MAX as u64
}
pub fn valid_external_url(url: &str) -> bool {
    url.len() <= 2048
        && url
            .strip_prefix("https://")
            .is_some_and(|rest| !rest.is_empty())
        && url
            .bytes()
            .all(|byte| (0x21..=0x7e).contains(&byte) && !matches!(byte, b'\\' | b'"'))
}
