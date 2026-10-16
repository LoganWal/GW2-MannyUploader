use std::ffi::c_void;
use std::io::ErrorKind;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::fs::MetadataExt;
use std::path::{Path, PathBuf};

use manny_core::application::{CandidateError, DirectoryChange, DirectoryChangeMonitor, StopToken};

type Handle = *mut c_void;
const INVALID_HANDLE: Handle = -1_isize as Handle;
const CHANGE_FILTER: u32 = 0x01 | 0x02 | 0x08 | 0x10 | 0x40;
const WAIT_TIMEOUT: u32 = 258;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn FindFirstChangeNotificationW(path: *const u16, recursive: i32, filter: u32) -> Handle;
    fn FindNextChangeNotification(handle: Handle) -> i32;
    fn FindCloseChangeNotification(handle: Handle) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
}

struct ChangeHandle(Handle);

// SAFETY: A notification handle can move between threads. Its sole owner only queries, rearms,
// or closes it through exclusive mutable monitor access. It is never shared with callbacks.
unsafe impl Send for ChangeHandle {}

impl Drop for ChangeHandle {
    fn drop(&mut self) {
        // SAFETY: Construction admits only a valid owned notification handle, closed exactly once.
        unsafe {
            FindCloseChangeNotification(self.0);
        }
    }
}

#[derive(Default)]
pub struct WindowsDirectoryMonitor {
    root: PathBuf,
    recursive: bool,
    handle: Option<ChangeHandle>,
}

impl DirectoryChangeMonitor for WindowsDirectoryMonitor {
    fn reconfigure(&mut self, root: &Path, recursive: bool) -> Result<(), CandidateError> {
        if root.as_os_str().is_empty() || root.as_os_str().encode_wide().any(|unit| unit == 0) {
            return Err(CandidateError::InvalidConfiguration);
        }
        let root = std::path::absolute(root).map_err(|_| CandidateError::InvalidConfiguration)?;
        self.handle = None;
        self.root = root;
        self.recursive = recursive;
        Ok(())
    }

    fn poll(&mut self, stop: &StopToken) -> Result<DirectoryChange, CandidateError> {
        if stop.is_stop_requested() {
            return Err(CandidateError::Cancelled);
        }
        if self.root.as_os_str().is_empty() {
            return Err(CandidateError::InvalidConfiguration);
        }
        // Notifications cover children, so root disappearance must also be checked explicitly.
        let metadata = match std::fs::symlink_metadata(&self.root) {
            Ok(metadata) => metadata,
            Err(error) => {
                self.handle = None;
                return if error.kind() == ErrorKind::NotFound {
                    Ok(DirectoryChange::Unavailable)
                } else {
                    Err(CandidateError::ScanFailed)
                };
            }
        };
        if !metadata.is_dir() || metadata.file_attributes() & 0x400 != 0 {
            self.handle = None;
            return Err(CandidateError::ScanFailed);
        }
        let Some(handle) = &self.handle else {
            // canonicalize supplies a Windows extended-length path and keeps Unicode lossless.
            let path = std::fs::canonicalize(&self.root).map_err(|_| CandidateError::ScanFailed)?;
            let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
            // SAFETY: wide is a live NUL-terminated UTF-16 path. BOOL/DWORD and HANDLE match Win32.
            let raw = unsafe {
                FindFirstChangeNotificationW(
                    wide.as_ptr(),
                    i32::from(self.recursive),
                    CHANGE_FILTER,
                )
            };
            if raw == INVALID_HANDLE || raw.is_null() {
                return Err(CandidateError::ScanFailed);
            }
            self.handle = Some(ChangeHandle(raw));
            return Ok(DirectoryChange::Changed);
        };
        // SAFETY: The owned handle remains open for this call. A zero timeout never blocks.
        match unsafe { WaitForSingleObject(handle.0, 0) } {
            WAIT_TIMEOUT => Ok(DirectoryChange::Unchanged),
            // SAFETY: The wait signaled this valid notification handle before rearming it.
            0 if unsafe { FindNextChangeNotification(handle.0) } != 0 => {
                Ok(DirectoryChange::Changed)
            }
            _ => {
                self.handle = None;
                Err(CandidateError::ScanFailed)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use manny_core::application::CancellationSource;
    use std::fs;
    use std::io::Write;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{Duration, Instant};

    struct Tree(PathBuf);
    impl Tree {
        fn new() -> Self {
            static NEXT: AtomicU64 = AtomicU64::new(0);
            let root = std::env::temp_dir().join(format!(
                "manny-notify-{}-{}-\u{96ea}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&root).unwrap();
            Self(root)
        }
    }
    impl Drop for Tree {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn wait_for(monitor: &mut WindowsDirectoryMonitor, wanted: DirectoryChange) {
        let stop = CancellationSource::default().token();
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            if monitor.poll(&stop).unwrap() == wanted {
                return;
            }
            assert!(
                Instant::now() < deadline,
                "native notification deadline exceeded"
            );
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    #[test]
    fn native_monitor_observes_unicode_nested_create_modify_rename_and_delete() {
        let tree = Tree::new();
        let nested = tree.0.join("nested");
        fs::create_dir(&nested).unwrap();
        let mut monitor = WindowsDirectoryMonitor::default();
        monitor.reconfigure(&tree.0, true).unwrap();
        wait_for(&mut monitor, DirectoryChange::Changed);
        wait_for(&mut monitor, DirectoryChange::Unchanged);
        let path = nested.join("encounter.zevtc");
        fs::write(&path, b"initial").unwrap();
        wait_for(&mut monitor, DirectoryChange::Changed);
        wait_for(&mut monitor, DirectoryChange::Unchanged);
        let mut file = fs::OpenOptions::new().append(true).open(&path).unwrap();
        file.write_all(b"append").unwrap();
        file.sync_all().unwrap();
        drop(file);
        wait_for(&mut monitor, DirectoryChange::Changed);
        wait_for(&mut monitor, DirectoryChange::Unchanged);
        let renamed = nested.join("renamed.zevtc");
        fs::rename(&path, &renamed).unwrap();
        wait_for(&mut monitor, DirectoryChange::Changed);
        wait_for(&mut monitor, DirectoryChange::Unchanged);
        fs::remove_file(renamed).unwrap();
        wait_for(&mut monitor, DirectoryChange::Changed);
        monitor.reconfigure(&tree.0, false).unwrap();
        assert!(monitor.handle.is_none());
    }

    #[test]
    fn missing_root_recovers_and_invalid_configuration_or_cancellation_preserves_handle() {
        let tree = Tree::new();
        let root = tree.0.join("later");
        let stop = CancellationSource::default().token();
        let mut monitor = WindowsDirectoryMonitor::default();
        assert_eq!(
            monitor.poll(&stop),
            Err(CandidateError::InvalidConfiguration)
        );
        monitor.reconfigure(&root, false).unwrap();
        assert_eq!(monitor.poll(&stop), Ok(DirectoryChange::Unavailable));
        fs::create_dir(&root).unwrap();
        assert_eq!(monitor.poll(&stop), Ok(DirectoryChange::Changed));
        let handle = monitor.handle.as_ref().unwrap().0;
        assert_eq!(
            monitor.reconfigure(Path::new(""), false),
            Err(CandidateError::InvalidConfiguration)
        );
        assert_eq!(
            monitor.reconfigure(Path::new("nul\0suffix"), false),
            Err(CandidateError::InvalidConfiguration)
        );
        assert_eq!(monitor.handle.as_ref().unwrap().0, handle);
        let cancelled = CancellationSource::default();
        cancelled.request_stop();
        assert_eq!(
            monitor.poll(&cancelled.token()),
            Err(CandidateError::Cancelled)
        );
        assert_eq!(monitor.handle.as_ref().unwrap().0, handle);
        fs::remove_dir(&root).unwrap();
        assert_eq!(monitor.poll(&stop), Ok(DirectoryChange::Unavailable));
        assert!(monitor.handle.is_none());
        fs::create_dir(&root).unwrap();
        assert_eq!(monitor.poll(&stop), Ok(DirectoryChange::Changed));
    }
}
