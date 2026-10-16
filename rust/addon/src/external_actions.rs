use manny_core::application::{ExternalActionError, ExternalActionLauncher, valid_external_url};
use std::{ffi::c_void, os::windows::ffi::OsStrExt, path::Path, ptr};

#[link(name = "shell32")]
unsafe extern "system" {
    fn ShellExecuteW(
        window: *mut c_void,
        operation: *const u16,
        file: *const u16,
        parameters: *const u16,
        directory: *const u16,
        show: i32,
    ) -> *mut c_void;
}
pub struct WindowsExternalActionLauncher;
impl ExternalActionLauncher for WindowsExternalActionLauncher {
    fn open_url(&mut self, url: &str) -> Result<(), ExternalActionError> {
        if !valid_external_url(url) {
            return Err(ExternalActionError::InvalidTarget);
        }
        launch(url.encode_utf16(), execute)
    }
    fn open_directory(&mut self, directory: &Path) -> Result<(), ExternalActionError> {
        launch(directory.as_os_str().encode_wide(), execute)
    }
}
fn launch(
    value: impl Iterator<Item = u16>,
    execute: impl FnOnce(&[u16]) -> isize,
) -> Result<(), ExternalActionError> {
    let mut wide = Vec::new();
    for unit in value {
        if unit == 0 || wide.len() >= 32766 {
            return Err(ExternalActionError::InvalidTarget);
        }
        wide.push(unit);
    }
    if wide.is_empty() {
        return Err(ExternalActionError::InvalidTarget);
    }
    wide.push(0);
    if execute(&wide) > 32 {
        Ok(())
    } else {
        Err(ExternalActionError::LaunchFailed)
    }
}
fn execute(target: &[u16]) -> isize {
    let open = [b'o' as u16, b'p' as u16, b'e' as u16, b'n' as u16, 0];
    // SAFETY: Both UTF-16 buffers are terminated and alive through the call. No parameters or
    // command interpreter are supplied. ShellExecuteW returns a status value, not an owned handle.
    unsafe {
        ShellExecuteW(
            ptr::null_mut(),
            open.as_ptr(),
            target.as_ptr(),
            ptr::null(),
            ptr::null(),
            1,
        ) as isize
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unicode_targets_are_terminated_without_becoming_command_lines() {
        let target = "C:/synthetic/\u{65e5}\u{672c}/logs";
        assert_eq!(
            launch(target.encode_utf16(), |wide| {
                assert_eq!(wide, target.encode_utf16().chain([0]).collect::<Vec<_>>());
                33
            }),
            Ok(())
        );
    }
    #[test]
    fn invalid_targets_never_call_the_shell() {
        for target in [String::new(), "a\0b".into(), "a".repeat(32767)] {
            assert_eq!(
                launch(target.encode_utf16(), |_| panic!(
                    "invalid target reached the shell"
                )),
                Err(ExternalActionError::InvalidTarget)
            );
        }
        for code in [0, 2, 5, 31, 32] {
            assert_eq!(
                launch("fixture".encode_utf16(), |_| code),
                Err(ExternalActionError::LaunchFailed)
            );
        }
    }
}
