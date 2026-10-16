use std::ffi::{c_char, c_void};
use std::ptr;

use manny_core::application::{
    MAX_PROTECTED_RECORD_BYTES, MAX_PROTECTION_PLAINTEXT_BYTES, SecretProtectionError,
    SecretProtectionErrorCode, SecretProtectionMode, SecretProtector, SecretValue,
};
use zeroize::Zeroize;

const ENTROPY: &[u8] = b"GW2-MannyUploader/secret-v1";
const UI_FORBIDDEN: u32 = 1;

#[repr(C)]
struct DataBlob {
    len: u32,
    data: *mut u8,
}

#[link(name = "crypt32")]
unsafe extern "system" {
    fn CryptProtectData(
        input: *const DataBlob,
        description: *const u16,
        entropy: *const DataBlob,
        reserved: *mut c_void,
        prompt: *const c_void,
        flags: u32,
        output: *mut DataBlob,
    ) -> i32;
    fn CryptUnprotectData(
        input: *const DataBlob,
        description: *mut *mut u16,
        entropy: *const DataBlob,
        reserved: *mut c_void,
        prompt: *const c_void,
        flags: u32,
        output: *mut DataBlob,
    ) -> i32;
}
#[link(name = "kernel32")]
unsafe extern "system" {
    fn LocalFree(memory: *mut c_void) -> *mut c_void;
    fn GetLastError() -> u32;
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
}

pub struct DpapiSecretProtector {
    mode: SecretProtectionMode,
}

impl DpapiSecretProtector {
    pub fn new() -> Result<Self, SecretProtectionError> {
        let name: Vec<_> = "ntdll.dll\0".encode_utf16().collect();
        // SAFETY: The terminated UTF-16 name remains alive. GetModuleHandle borrows the loaded module.
        let module = unsafe { GetModuleHandleW(name.as_ptr()) };
        if module.is_null() {
            // SAFETY: Read the thread-local error immediately after the failed call.
            let system_error = Some(unsafe { GetLastError() });
            return Err(SecretProtectionError {
                code: SecretProtectionErrorCode::UnsupportedEnvironment,
                system_error,
            });
        }
        // SAFETY: The module is loaded, the export name is terminated, and the address is only tested.
        let wine = unsafe { GetProcAddress(module, c"wine_get_version".as_ptr()) };
        Ok(Self {
            mode: if wine.is_null() {
                SecretProtectionMode::NativeUserScoped
            } else {
                SecretProtectionMode::WineCompatibility
            },
        })
    }
}

impl SecretProtector for DpapiSecretProtector {
    fn mode(&self) -> SecretProtectionMode {
        self.mode
    }

    fn protect(&self, plaintext: &[u8]) -> Result<Vec<u8>, SecretProtectionError> {
        if plaintext.len() > MAX_PROTECTION_PLAINTEXT_BYTES {
            return Err(error(SecretProtectionErrorCode::InputTooLarge));
        }
        let input = input_blob(plaintext);
        let entropy = input_blob(ENTROPY);
        let description: Vec<_> = "GW2 Manny Uploader credential\0".encode_utf16().collect();
        let mut output = LocalBlob::new();
        // SAFETY: Input and entropy are bounded live read-only buffers. Description is terminated.
        // DPAPI owns the returned allocation, which LocalBlob wipes and frees on every exit path.
        let succeeded = unsafe {
            CryptProtectData(
                &input,
                description.as_ptr(),
                &entropy,
                ptr::null_mut(),
                ptr::null(),
                UI_FORBIDDEN,
                &mut output.0,
            )
        };
        if succeeded == 0 {
            // SAFETY: Capture the numeric error before cleanup makes any other Windows call.
            let system_error = Some(unsafe { GetLastError() });
            return Err(SecretProtectionError {
                code: SecretProtectionErrorCode::ProtectionFailed,
                system_error,
            });
        }
        Ok(output.bytes(MAX_PROTECTED_RECORD_BYTES)?.to_vec())
    }

    fn unprotect(&self, ciphertext: &[u8]) -> Result<SecretValue, SecretProtectionError> {
        if ciphertext.len() > MAX_PROTECTED_RECORD_BYTES {
            return Err(error(SecretProtectionErrorCode::InputTooLarge));
        }
        let input = input_blob(ciphertext);
        let entropy = input_blob(ENTROPY);
        let mut output = LocalBlob::new();
        // SAFETY: Both inputs stay alive and DPAPI treats them as read-only. No description allocation
        // or prompt is requested. LocalBlob owns all returned plaintext until it is wiped and freed.
        let succeeded = unsafe {
            CryptUnprotectData(
                &input,
                ptr::null_mut(),
                &entropy,
                ptr::null_mut(),
                ptr::null(),
                UI_FORBIDDEN,
                &mut output.0,
            )
        };
        if succeeded == 0 {
            // SAFETY: Capture the numeric error immediately after the failed DPAPI call.
            let system_error = Some(unsafe { GetLastError() });
            return Err(SecretProtectionError {
                code: SecretProtectionErrorCode::UnprotectionFailed,
                system_error,
            });
        }
        Ok(SecretValue::from_bytes(
            output.bytes(MAX_PROTECTION_PLAINTEXT_BYTES)?.to_vec(),
        ))
    }
}

fn input_blob(bytes: &[u8]) -> DataBlob {
    // The ABI uses a mutable pointer for these documented input-only buffers.
    DataBlob {
        len: bytes.len() as u32,
        data: bytes.as_ptr().cast_mut(),
    }
}
fn error(code: SecretProtectionErrorCode) -> SecretProtectionError {
    SecretProtectionError {
        code,
        system_error: None,
    }
}

struct LocalBlob(DataBlob);
impl LocalBlob {
    fn new() -> Self {
        Self(DataBlob {
            len: 0,
            data: ptr::null_mut(),
        })
    }
    fn bytes(&self, limit: usize) -> Result<&[u8], SecretProtectionError> {
        if self.0.len as usize > limit {
            return Err(error(SecretProtectionErrorCode::OutputTooLarge));
        }
        if self.0.len == 0 {
            return Ok(&[]);
        }
        if self.0.data.is_null() {
            return Err(error(SecretProtectionErrorCode::UnprotectionFailed));
        }
        // SAFETY: Successful DPAPI output owns len initialized bytes until this guard is dropped.
        Ok(unsafe { std::slice::from_raw_parts(self.0.data, self.0.len as usize) })
    }
}
impl Drop for LocalBlob {
    fn drop(&mut self) {
        if !self.0.data.is_null() {
            // SAFETY: The DPAPI allocation is exclusively owned, writable for len bytes, and bounded
            // by its u32 length on this Windows x64 target. Wipe even when output validation failed.
            unsafe { std::slice::from_raw_parts_mut(self.0.data, self.0.len as usize) }.zeroize();
            // SAFETY: DPAPI allocates with LocalAlloc. This guard frees the allocation exactly once.
            unsafe { LocalFree(self.0.data.cast()) };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::file_commit::WindowsFileCommit;
    use manny_core::application::{
        FileError, MAX_SECRET_BYTES, SecretId, SecretStore, SecretStoreErrorCode,
    };
    use manny_core::config::ProtectedFileSecretStore;
    use manny_core::filesystem::{FilesystemStore, temporary_path};
    use std::fs;
    use std::os::windows::fs::OpenOptionsExt;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    const MARKER: &[u8] = b"synthetic-native-DPAPI-marker";

    fn protector() -> DpapiSecretProtector {
        let protector = DpapiSecretProtector::new().unwrap();
        if std::env::var_os("MANNY_REQUIRE_NATIVE_DPAPI").as_deref()
            == Some(std::ffi::OsStr::new("1"))
        {
            assert_eq!(protector.mode(), SecretProtectionMode::NativeUserScoped);
        }
        if std::env::var_os("MANNY_REQUIRE_WINE_DPAPI").as_deref()
            == Some(std::ffi::OsStr::new("1"))
        {
            assert_eq!(protector.mode(), SecretProtectionMode::WineCompatibility);
        }
        protector
    }

    #[test]
    fn real_dpapi_round_trip_tamper_rejection_and_input_bounds() {
        let protector = protector();
        let protected = protector.protect(MARKER).unwrap();
        assert!(!protected.windows(MARKER.len()).any(|bytes| bytes == MARKER));
        assert_eq!(protector.unprotect(&protected).unwrap().expose(), MARKER);
        let mut tampered = protected.clone();
        let middle = tampered.len() / 2;
        tampered[middle] ^= 0x80;
        assert!(protector.unprotect(&tampered).is_err());
        assert!(protector.unprotect(&[]).is_err());
        assert!(
            protector
                .unprotect(&protected[..protected.len() - 1])
                .is_err()
        );
        assert_eq!(
            protector
                .protect(&vec![0; MAX_PROTECTION_PLAINTEXT_BYTES + 1])
                .unwrap_err()
                .code,
            SecretProtectionErrorCode::InputTooLarge
        );
        assert_eq!(
            protector
                .unprotect(&vec![0; MAX_PROTECTED_RECORD_BYTES + 1])
                .unwrap_err()
                .code,
            SecretProtectionErrorCode::InputTooLarge
        );
        let maximum = SecretValue::from_bytes(vec![0x42; MAX_PROTECTION_PLAINTEXT_BYTES]);
        let protected = protector.protect(maximum.expose()).unwrap();
        assert_eq!(
            protector.unprotect(&protected).unwrap().expose(),
            maximum.expose()
        );
    }

    #[test]
    fn real_protected_store_replacement_recovery_and_erasure_on_unicode_paths() {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let root = std::env::temp_dir().join(format!(
            "manny-dpapi-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&root).unwrap();
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
        let _cleanup = Cleanup(root.clone());
        let directory = root
            .join("a".repeat(100))
            .join("b".repeat(100))
            .join("\u{96ea}");
        let mut store = ProtectedFileSecretStore::new(
            Box::new(FilesystemStore::new(WindowsFileCommit)),
            Box::new(protector()),
            directory.clone(),
        )
        .unwrap();
        for id in SecretId::ALL {
            let value = SecretValue::from_bytes(MARKER.to_vec());
            store.store(id, &value).unwrap();
            assert_eq!(store.load(id).unwrap().expose(), MARKER);
            let path = store.record_path(id);
            let previous = fs::read(&path).unwrap();
            assert!(!previous.windows(MARKER.len()).any(|bytes| bytes == MARKER));
            let locked = fs::OpenOptions::new()
                .read(true)
                .share_mode(0)
                .open(&path)
                .unwrap();
            let replacement = SecretValue::from_bytes(vec![0xff; MAX_SECRET_BYTES]);
            assert_eq!(
                store.store(id, &replacement).unwrap_err().code,
                SecretStoreErrorCode::File(FileError::ReplaceFailed)
            );
            drop(locked);
            assert_eq!(fs::read(&path).unwrap(), previous);
            assert_eq!(store.load(id).unwrap().expose(), MARKER);
            assert!(temporary_path(&path).is_file());
            store.store(id, &replacement).unwrap();
            assert_eq!(store.load(id).unwrap().expose(), replacement.expose());
            assert!(!temporary_path(&path).exists());
            fs::write(&path, b"invalid protected record").unwrap();
            assert!(store.load(id).is_err());
            store.store(id, &value).unwrap();
            fs::write(temporary_path(&path), &previous).unwrap();
            store.erase(id).unwrap();
            store.erase(id).unwrap();
            assert_eq!(
                store.load(id).unwrap_err().code,
                SecretStoreErrorCode::NotFound
            );
            assert_eq!(fs::read_dir(&directory).unwrap().count(), 0);
        }
    }
}
