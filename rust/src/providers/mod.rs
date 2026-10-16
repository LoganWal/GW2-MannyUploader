mod async_upload_worker;
mod donbot;
mod dps_report;
mod json;
mod twitch;
mod wingman;
mod wire;
pub use wingman::*;

pub use async_upload_worker::{
    AsyncUploadWorker, UploadProcessor, WorkerError, WorkerOptions, WorkerSnapshot,
};
pub use donbot::*;
pub use dps_report::*;
pub use twitch::*;

impl From<WorkerError> for crate::application::ProviderConfigurationError {
    fn from(error: WorkerError) -> Self {
        match error {
            WorkerError::InvalidCapacity | WorkerError::InvalidParallelism => {
                Self::InvalidConfiguration
            }
            WorkerError::AllocationFailed | WorkerError::ThreadStartFailed => Self::ResourceLimit,
            WorkerError::Stopping => Self::Stopping,
        }
    }
}
