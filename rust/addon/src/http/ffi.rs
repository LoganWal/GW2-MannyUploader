//! Private libcurl 8.21.0 C ABI. Options retain the numeric values in the pinned curl.h.
use std::ffi::{c_char, c_int, c_long, c_void};

#[repr(C)]
pub struct Slist {
    pub data: *mut c_char,
    pub next: *mut Slist,
}
pub type DataCallback = unsafe extern "C" fn(*mut c_char, usize, usize, *mut c_void) -> usize;
pub type ProgressCallback = unsafe extern "C" fn(*mut c_void, i64, i64, i64, i64) -> c_int;

#[cfg(test)]
#[repr(C)]
pub struct VersionInfo {
    pub age: c_int,
    pub version: *const c_char,
    pub version_num: u32,
    pub host: *const c_char,
    pub features: c_int,
    pub ssl_version: *const c_char,
    pub ssl_version_num: c_long,
    pub libz_version: *const c_char,
    pub protocols: *const *const c_char,
}

unsafe extern "C" {
    pub fn curl_global_init(flags: c_long) -> c_int;
    pub fn curl_global_cleanup();
    pub fn curl_version() -> *const c_char;
    #[cfg(test)]
    pub fn curl_version_info(age: c_int) -> *const VersionInfo;
    pub fn curl_easy_init() -> *mut c_void;
    pub fn curl_easy_cleanup(handle: *mut c_void);
    pub fn curl_easy_setopt(handle: *mut c_void, option: c_int, ...) -> c_int;
    pub fn curl_easy_perform(handle: *mut c_void) -> c_int;
    pub fn curl_easy_getinfo(handle: *mut c_void, info: c_int, ...) -> c_int;
    pub fn curl_slist_append(list: *mut Slist, value: *const c_char) -> *mut Slist;
    pub fn curl_slist_free_all(list: *mut Slist);
}

pub const WRITEDATA: c_int = 10001;
pub const URL: c_int = 10002;
pub const READDATA: c_int = 10009;
pub const WRITEFUNCTION: c_int = 20011;
pub const READFUNCTION: c_int = 20012;
pub const LOW_SPEED_LIMIT: c_int = 19;
pub const LOW_SPEED_TIME: c_int = 20;
pub const HTTPHEADER: c_int = 10023;
pub const HEADERDATA: c_int = 10029;
pub const SSLVERSION: c_int = 32;
pub const CUSTOMREQUEST: c_int = 10036;
pub const VERBOSE: c_int = 41;
pub const NOPROGRESS: c_int = 43;
pub const FAILONERROR: c_int = 45;
pub const UPLOAD: c_int = 46;
pub const FOLLOWLOCATION: c_int = 52;
pub const XFERINFODATA: c_int = 10057;
pub const SSL_VERIFYPEER: c_int = 64;
pub const MAXREDIRS: c_int = 68;
pub const HEADERFUNCTION: c_int = 20079;
pub const SSL_VERIFYHOST: c_int = 81;
pub const NOSIGNAL: c_int = 99;
pub const UNRESTRICTED_AUTH: c_int = 105;
pub const INFILESIZE_LARGE: c_int = 30115;
pub const MAXFILESIZE_LARGE: c_int = 30117;
pub const TIMEOUT_MS: c_int = 155;
pub const CONNECTTIMEOUT_MS: c_int = 156;
pub const NOPROXY: c_int = 10177;
pub const XFERINFOFUNCTION: c_int = 20219;
pub const SUPPRESS_CONNECT_HEADERS: c_int = 265;
pub const PROTOCOLS_STR: c_int = 10318;
pub const REDIR_PROTOCOLS_STR: c_int = 10319;
pub const RESPONSE_CODE: c_int = 0x200002;
pub const READ_ABORT: usize = 0x10000000;
