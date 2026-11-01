use std::ffi::{c_char, c_void};

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
    fn GetLastError() -> u32;
}

pub(crate) fn is_wine() -> Result<bool, u32> {
    let name: Vec<_> = "ntdll.dll\0".encode_utf16().collect();
    // SAFETY: The terminated UTF-16 name remains alive. The returned module is borrowed.
    let module = unsafe { GetModuleHandleW(name.as_ptr()) };
    if module.is_null() {
        // SAFETY: Read the thread-local error immediately after the failed call.
        return Err(unsafe { GetLastError() });
    }
    // SAFETY: The module is loaded, the export name is terminated, and the address is only tested.
    Ok(!unsafe { GetProcAddress(module, c"wine_get_version".as_ptr()) }.is_null())
}
