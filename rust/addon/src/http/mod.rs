mod ffi;
#[cfg(test)]
mod tests;

use std::ffi::{CStr, CString, c_char, c_int, c_long, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::NonNull;
use std::sync::Mutex;
use zeroize::{Zeroize, Zeroizing};

use manny_core::application::*;
use manny_core::http::{ExactLengthBody, ResponseCollector};

static RUNTIME_USERS: Mutex<usize> = Mutex::new(0);
struct Runtime;
impl Runtime {
    fn new() -> Result<Self, HttpError> {
        let mut users = RUNTIME_USERS.lock().unwrap_or_else(|e| e.into_inner());
        let next = users
            .checked_add(1)
            .ok_or(HttpError::new(HttpErrorCode::Internal))?;
        if *users == 0 {
            // SAFETY: Initialization and cleanup are serialized. No easy handle exists without a user.
            let code = unsafe { ffi::curl_global_init(3) };
            if code != 0 {
                return Err(backend_error(code, HttpErrorCode::InitializationFailed));
            }
        }
        *users = next;
        Ok(Self)
    }
}
impl Drop for Runtime {
    fn drop(&mut self) {
        let mut users = RUNTIME_USERS.lock().unwrap_or_else(|e| e.into_inner());
        *users -= 1;
        if *users == 0 {
            // SAFETY: All execute calls borrow their client. The last client cannot drop mid-request.
            unsafe { ffi::curl_global_cleanup() };
        }
    }
}

pub struct CurlHttpClient {
    policy: HttpTransportPolicy,
    _runtime: Runtime,
}
impl CurlHttpClient {
    pub fn new(policy: HttpTransportPolicy) -> Result<Self, HttpError> {
        Ok(Self {
            policy,
            _runtime: Runtime::new()?,
        })
    }
    pub fn backend_version(&self) -> Result<String, HttpError> {
        let _lock = RUNTIME_USERS.lock().unwrap_or_else(|e| e.into_inner());
        // SAFETY: This client holds global initialization. libcurl returns a static NUL-terminated string.
        let pointer = unsafe { ffi::curl_version() };
        if pointer.is_null() {
            return Err(HttpError::new(HttpErrorCode::Internal));
        }
        // SAFETY: The non-null pointer is the library-owned version string and is not freed here.
        Ok(unsafe { CStr::from_ptr(pointer) }
            .to_string_lossy()
            .into_owned())
    }
    fn perform(
        &self,
        mut request: HttpRequest,
        stop: &StopToken,
    ) -> Result<HttpResponse, HttpError> {
        if stop.is_stop_requested() {
            return Err(HttpError::new(HttpErrorCode::Cancelled));
        }
        validate_http_request(&request, self.policy)?;
        let body = request
            .body
            .take()
            .map(ExactLengthBody::new)
            .transpose()
            .map_err(body_error)?;
        let mut transfer = Transfer {
            stop: stop.clone(),
            body,
            response: ResponseCollector::new(request.response_limits)?,
            failure: None,
        };
        let mut headers = Headers(std::ptr::null_mut());
        for header in &request.headers {
            headers.append(header.name().as_bytes(), header.expose_value())?;
        }
        if transfer.body.is_some() {
            headers.insert(c"Expect:")?;
        }
        // Declared last and explicitly dropped before moving transfer. Cleanup may still call callbacks.
        let easy = Easy::new()?;
        let mut url = Zeroizing::new(Vec::with_capacity(request.url.len() + 1));
        url.extend_from_slice(request.url.as_bytes());
        url.push(0);
        request.url.zeroize();
        let method = CString::new(request.method.name())
            .map_err(|_| HttpError::new(HttpErrorCode::Internal))?;
        let protocols = if self.policy == HttpTransportPolicy::HttpsOnly {
            c"https"
        } else {
            c"http,https"
        };
        let pointer = (&mut transfer as *mut Transfer).cast::<c_void>();
        // SAFETY: The handle is uniquely owned. Each option uses its documented C vararg type.
        // String options are copied synchronously. List/context pointers live through easy cleanup.
        unsafe {
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::URL,
                url.as_ptr().cast::<c_char>(),
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::CUSTOMREQUEST,
                method.as_ptr(),
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::PROTOCOLS_STR,
                protocols.as_ptr(),
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::REDIR_PROTOCOLS_STR,
                protocols.as_ptr(),
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::HTTPHEADER,
                headers.0,
            ))?;
            if self.policy == HttpTransportPolicy::PlaintextLoopbackForTests {
                check(ffi::curl_easy_setopt(
                    easy.0.as_ptr(),
                    ffi::NOPROXY,
                    c"*".as_ptr(),
                ))?;
            }
            for option in [
                ffi::READDATA,
                ffi::WRITEDATA,
                ffi::HEADERDATA,
                ffi::XFERINFODATA,
            ] {
                check(ffi::curl_easy_setopt(easy.0.as_ptr(), option, pointer))?;
            }
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::READFUNCTION,
                read_callback as ffi::DataCallback,
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::WRITEFUNCTION,
                write_callback as ffi::DataCallback,
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::HEADERFUNCTION,
                header_callback as ffi::DataCallback,
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::XFERINFOFUNCTION,
                progress_callback as ffi::ProgressCallback,
            ))?;
            check(ffi::curl_easy_setopt(
                easy.0.as_ptr(),
                ffi::MAXFILESIZE_LARGE,
                request.response_limits.max_body_bytes as i64,
            ))?;
            if let Some(body) = &transfer.body {
                check(ffi::curl_easy_setopt(
                    easy.0.as_ptr(),
                    ffi::INFILESIZE_LARGE,
                    body.content_length() as i64,
                ))?;
                easy.long(ffi::UPLOAD, 1)?;
            }
        }
        for (option, value) in [
            (ffi::FOLLOWLOCATION, 0),
            (ffi::MAXREDIRS, 0),
            (ffi::UNRESTRICTED_AUTH, 0),
            (ffi::NOSIGNAL, 1),
            (ffi::VERBOSE, 0),
            (ffi::SSL_VERIFYPEER, 1),
            (ffi::SSL_VERIFYHOST, 2),
            (ffi::SSLVERSION, 6),
            (ffi::NOPROGRESS, 0),
            (ffi::FAILONERROR, 0),
            (ffi::SUPPRESS_CONNECT_HEADERS, 1),
            (ffi::LOW_SPEED_LIMIT, 1),
            (
                ffi::CONNECTTIMEOUT_MS,
                request.timeouts.connect.as_millis() as c_long,
            ),
            (
                ffi::TIMEOUT_MS,
                request.timeouts.operation.as_millis() as c_long,
            ),
            (
                ffi::LOW_SPEED_TIME,
                request.timeouts.stalled_transfer.as_millis().div_ceil(1000) as c_long,
            ),
        ] {
            easy.long(option, value)?;
        }
        // SAFETY: All callback data remains pinned on this stack for this synchronous call.
        let code = unsafe { ffi::curl_easy_perform(easy.0.as_ptr()) };
        let mut status: c_long = 0;
        // SAFETY: CURLINFO_RESPONSE_CODE writes a C long into the live output pointer.
        let info = unsafe {
            ffi::curl_easy_getinfo(
                easy.0.as_ptr(),
                ffi::RESPONSE_CODE,
                &mut status as *mut c_long,
            )
        };
        drop(easy);
        if let Some(error) = transfer.failure {
            return Err(error);
        }
        if stop.is_stop_requested() {
            return Err(HttpError::new(HttpErrorCode::Cancelled));
        }
        if code != 0 {
            return Err(map_backend_error(code));
        }
        check(info)?;
        if transfer
            .body
            .as_ref()
            .is_some_and(|body| !body.is_complete())
        {
            return Err(body_error(HttpBodyError::new(
                HttpBodyErrorCode::ReadFailed,
            )));
        }
        let status =
            u16::try_from(status).map_err(|_| HttpError::new(HttpErrorCode::ProtocolError))?;
        transfer.response.finish(status, stop)
    }
}
impl HttpClient for CurlHttpClient {
    fn execute(&self, request: HttpRequest, stop: &StopToken) -> Result<HttpResponse, HttpError> {
        catch_unwind(AssertUnwindSafe(|| self.perform(request, stop)))
            .unwrap_or(Err(HttpError::new(HttpErrorCode::Internal)))
    }
}
struct Easy(NonNull<c_void>);
impl Easy {
    fn new() -> Result<Self, HttpError> {
        // SAFETY: Construction occurs only during execute while its client owns global initialization.
        NonNull::new(unsafe { ffi::curl_easy_init() })
            .map(Self)
            .ok_or(HttpError::new(HttpErrorCode::InitializationFailed))
    }
    fn long(&self, option: c_int, value: c_long) -> Result<(), HttpError> {
        debug_assert!((0..10000).contains(&option));
        // SAFETY: Call sites supply CURLOPTTYPE_LONG options and the exact C long argument type.
        check(unsafe { ffi::curl_easy_setopt(self.0.as_ptr(), option, value) })
    }
}
impl Drop for Easy {
    fn drop(&mut self) {
        // SAFETY: This guard uniquely owns the handle. Callback context outlives cleanup.
        unsafe { ffi::curl_easy_cleanup(self.0.as_ptr()) };
    }
}
struct Headers(*mut ffi::Slist);
impl Headers {
    fn append(&mut self, name: &[u8], value: &[u8]) -> Result<(), HttpError> {
        let mut line = Zeroizing::new(Vec::with_capacity(name.len() + value.len() + 3));
        line.extend_from_slice(name);
        if value.is_empty() {
            // libcurl uses a semicolon to send an empty value. A colon would suppress the header.
            line.push(b';');
        } else {
            line.push(b':');
            line.push(b' ');
            line.extend_from_slice(value);
        }
        line.push(0);
        self.insert(
            CStr::from_bytes_with_nul(&line)
                .map_err(|_| HttpError::new(HttpErrorCode::InvalidRequest))?,
        )
    }
    fn insert(&mut self, line: &CStr) -> Result<(), HttpError> {
        // SAFETY: Validation excluded embedded NUL. libcurl copies the terminated string and keeps
        // the old list unchanged on allocation failure. The temporary copy wipes on every path.
        let next = unsafe { ffi::curl_slist_append(self.0, line.as_ptr()) };
        if next.is_null() {
            return Err(HttpError::new(HttpErrorCode::Internal));
        }
        self.0 = next;
        Ok(())
    }
}
impl Drop for Headers {
    fn drop(&mut self) {
        // SAFETY: Nodes and NUL-terminated writable strings are exclusively owned libcurl allocations.
        // Easy cleanup precedes this guard. Wipe the copies before freeing the whole list once.
        unsafe {
            let mut node = self.0;
            while !node.is_null() {
                let data = (*node).data;
                if !data.is_null() {
                    let length = CStr::from_ptr(data).to_bytes().len();
                    std::slice::from_raw_parts_mut(data.cast::<u8>(), length).zeroize();
                }
                node = (*node).next;
            }
            ffi::curl_slist_free_all(self.0);
        }
    }
}
struct Transfer {
    stop: StopToken,
    body: Option<ExactLengthBody>,
    response: ResponseCollector,
    failure: Option<HttpError>,
}
impl Transfer {
    fn callback<T>(
        &mut self,
        action: impl FnOnce(&mut Self) -> Result<T, HttpError>,
    ) -> Result<T, HttpError> {
        if let Some(error) = self.failure {
            return Err(error);
        }
        let result = if self.stop.is_stop_requested() {
            Err(HttpError::new(HttpErrorCode::Cancelled))
        } else {
            catch_unwind(AssertUnwindSafe(|| action(self)))
                .unwrap_or(Err(HttpError::new(HttpErrorCode::Internal)))
        };
        if let Err(error) = result {
            self.failure = Some(error);
        }
        result
    }
}
unsafe extern "C" fn read_callback(
    data: *mut c_char,
    size: usize,
    count: usize,
    context: *mut c_void,
) -> usize {
    // SAFETY: execute registers a live unique Transfer, and libcurl calls synchronously without reentry.
    let transfer = unsafe { &mut *context.cast::<Transfer>() };
    transfer
        .callback(|transfer| {
            let capacity = size
                .checked_mul(count)
                .filter(|&n| n <= isize::MAX as usize)
                .ok_or(HttpError::new(HttpErrorCode::Internal))?;
            if capacity == 0 {
                return Ok(0);
            }
            if data.is_null() {
                return Err(HttpError::new(HttpErrorCode::Internal));
            }
            // SAFETY: libcurl provides capacity writable bytes for this callback only.
            let bytes = unsafe { std::slice::from_raw_parts_mut(data.cast::<u8>(), capacity) };
            transfer
                .body
                .as_mut()
                .ok_or(HttpError::new(HttpErrorCode::Internal))?
                .read(bytes, &transfer.stop)
                .map_err(body_error)
        })
        .unwrap_or(ffi::READ_ABORT)
}
unsafe fn receive(
    data: *mut c_char,
    size: usize,
    count: usize,
    context: *mut c_void,
    header: bool,
) -> usize {
    // SAFETY: execute registers a live unique Transfer for synchronous callbacks.
    let transfer = unsafe { &mut *context.cast::<Transfer>() };
    transfer
        .callback(|transfer| {
            let length = size
                .checked_mul(count)
                .filter(|&n| n <= isize::MAX as usize)
                .ok_or(HttpError::new(HttpErrorCode::Internal))?;
            if length == 0 {
                return Ok(0);
            }
            if data.is_null() {
                return Err(HttpError::new(HttpErrorCode::Internal));
            }
            // SAFETY: libcurl provides length readable bytes that remain valid for this callback.
            let bytes = unsafe { std::slice::from_raw_parts(data.cast::<u8>(), length) };
            if header {
                transfer.response.header(bytes, &transfer.stop)?;
            } else {
                transfer.response.body(bytes, &transfer.stop)?;
            }
            Ok(length)
        })
        .unwrap_or(usize::MAX)
}
unsafe extern "C" fn write_callback(
    data: *mut c_char,
    size: usize,
    count: usize,
    context: *mut c_void,
) -> usize {
    // SAFETY: Forward the unchanged libcurl callback arguments to the contained receiver.
    unsafe { receive(data, size, count, context, false) }
}
unsafe extern "C" fn header_callback(
    data: *mut c_char,
    size: usize,
    count: usize,
    context: *mut c_void,
) -> usize {
    // SAFETY: Forward the unchanged libcurl callback arguments to the contained receiver.
    unsafe { receive(data, size, count, context, true) }
}
unsafe extern "C" fn progress_callback(
    context: *mut c_void,
    _: i64,
    _: i64,
    _: i64,
    _: i64,
) -> c_int {
    // SAFETY: execute registers a live unique Transfer for synchronous callbacks.
    let transfer = unsafe { &mut *context.cast::<Transfer>() };
    if transfer.callback(|_| Ok(())).is_ok() {
        0
    } else {
        1
    }
}
fn check(code: c_int) -> Result<(), HttpError> {
    if code == 0 {
        Ok(())
    } else {
        Err(backend_error(code, HttpErrorCode::Internal))
    }
}
fn backend_error(code: c_int, category: HttpErrorCode) -> HttpError {
    HttpError {
        transport_code: Some(i64::from(code)),
        ..HttpError::new(category)
    }
}
fn body_error(error: HttpBodyError) -> HttpError {
    HttpError {
        code: if error.code == HttpBodyErrorCode::Cancelled {
            HttpErrorCode::Cancelled
        } else {
            HttpErrorCode::BodyReadFailed
        },
        transport_code: None,
        body_error: Some(error.code),
        system_error: error.system_error,
    }
}
fn map_backend_error(code: c_int) -> HttpError {
    backend_error(
        code,
        match code {
            1 | 3 => HttpErrorCode::InvalidRequest,
            2 => HttpErrorCode::InitializationFailed,
            5 | 6 => HttpErrorCode::NameResolutionFailed,
            7 => HttpErrorCode::ConnectionFailed,
            28 => HttpErrorCode::Timeout,
            35 | 51 | 53 | 54 | 58 | 59 | 60 | 66 | 77 | 80 | 82 | 83 | 90 | 91 | 98 => {
                HttpErrorCode::TlsFailed
            }
            55 | 65 => HttpErrorCode::SendFailed,
            56 | 52 | 18 => HttpErrorCode::ReceiveFailed,
            63 => HttpErrorCode::ResponseTooLarge,
            8 | 16 | 92 => HttpErrorCode::ProtocolError,
            _ => HttpErrorCode::Internal,
        },
    )
}
