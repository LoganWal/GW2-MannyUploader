//! Compiled only into explicitly requested lifecycle-test DLLs. Never performs network I/O.

use manny_core::{application::*, providers::DPS_REPORT_UPLOAD_URL};
use std::{
    sync::atomic::{AtomicBool, AtomicU64, Ordering},
    time::Duration,
};

static ENTERED: AtomicU64 = AtomicU64::new(0);
static EXITED: AtomicU64 = AtomicU64::new(0);
static ACTIVE: AtomicU64 = AtomicU64::new(0);
static CLIENTS: AtomicU64 = AtomicU64::new(0);
static FAILED: AtomicBool = AtomicBool::new(false);

/// Packed counters: entered, exited, active, live clients, each in 16 bits. Bit 63 signals failure.
#[unsafe(no_mangle)]
pub extern "system" fn MannyLifecycleTestState() -> u64 {
    ENTERED.load(Ordering::Acquire).min(0xffff)
        | (EXITED.load(Ordering::Acquire).min(0xffff) << 16)
        | (ACTIVE.load(Ordering::Acquire).min(0xffff) << 32)
        | (CLIENTS.load(Ordering::Acquire).min(0x7fff) << 48)
        | (u64::from(FAILED.load(Ordering::Acquire)) << 63)
}

#[unsafe(no_mangle)]
pub extern "system" fn MannyLifecycleTestJobs() -> u64 {
    std::panic::catch_unwind(crate::runtime::lifecycle_test_jobs).unwrap_or(u64::MAX)
}

pub(crate) struct BlockingHttp;
impl BlockingHttp {
    pub fn new() -> Self {
        CLIENTS.fetch_add(1, Ordering::AcqRel);
        Self
    }
}
impl Drop for BlockingHttp {
    fn drop(&mut self) {
        CLIENTS.fetch_sub(1, Ordering::AcqRel);
    }
}

struct Admission;
impl Admission {
    fn new() -> Self {
        ENTERED.fetch_add(1, Ordering::AcqRel);
        ACTIVE.fetch_add(1, Ordering::AcqRel);
        Self
    }
}
impl Drop for Admission {
    fn drop(&mut self) {
        ACTIVE.fetch_sub(1, Ordering::AcqRel);
        EXITED.fetch_add(1, Ordering::AcqRel);
    }
}
fn invalid() -> HttpError {
    FAILED.store(true, Ordering::Release);
    HttpError::new(HttpErrorCode::InvalidRequest)
}
impl HttpClient for BlockingHttp {
    fn execute(
        &self,
        mut request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, HttpError> {
        if request.url != DPS_REPORT_UPLOAD_URL
            || request.method != HttpMethod::Post
            || validate_http_request(&request, HttpTransportPolicy::HttpsOnly).is_err()
        {
            return Err(invalid());
        }
        let body = request.body.as_mut().ok_or_else(invalid)?;
        let length = body.content_length();
        if !(1..=65_536).contains(&length) {
            return Err(invalid());
        }
        let mut consumed = 0;
        let mut buffer = [0u8; 1024];
        loop {
            let count = body.read(&mut buffer, stop).map_err(|_| invalid())?;
            if count == 0 {
                break;
            }
            consumed += count as u64;
            if consumed > length {
                return Err(invalid());
            }
        }
        if consumed != length {
            return Err(invalid());
        }
        // Admission proves that the real parser/provider/multipart path reached this boundary.
        let _admission = Admission::new();
        if !stop.wait_for_stop(Duration::from_secs(30)) {
            return Err(invalid());
        }
        Err(HttpError::new(HttpErrorCode::Cancelled))
    }
}
