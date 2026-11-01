use manny_core::application::{RandomError, RandomSource};
use std::ffi::c_void;

#[link(name = "bcrypt")]
unsafe extern "system" {
    fn BCryptGenRandom(algorithm: *mut c_void, buffer: *mut u8, length: u32, flags: u32) -> i32;
}

pub struct WindowsRandom;
impl RandomSource for WindowsRandom {
    fn fill(&self, destination: &mut [u8]) -> Result<(), RandomError> {
        let length = u32::try_from(destination.len()).map_err(|_| RandomError::TooLarge)?;
        if length == 0 {
            return Ok(());
        }
        // SAFETY: The writable slice covers length bytes. A null algorithm and flag 2 select the
        // system-preferred RNG. BCrypt does not retain the buffer after this synchronous call.
        let status =
            unsafe { BCryptGenRandom(std::ptr::null_mut(), destination.as_mut_ptr(), length, 2) };
        if status >= 0 {
            Ok(())
        } else {
            Err(RandomError::Unavailable)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use manny_core::http::{MemoryBodySource, MultipartFormData, MultipartPart};
    #[test]
    fn native_randomness_constructs_bounded_multipart_boundaries() {
        WindowsRandom.fill(&mut []).unwrap();
        let form = MultipartFormData::new(
            vec![MultipartPart {
                name: "file".into(),
                filename: Some("upload.zevtc".into()),
                content_type: Some("application/octet-stream".into()),
                body: Box::new(MemoryBodySource::new(b"fixture".to_vec()).unwrap()),
            }],
            &WindowsRandom,
        )
        .unwrap();
        let boundary = form
            .content_type
            .strip_prefix("multipart/form-data; boundary=")
            .unwrap();
        assert_eq!(boundary.len(), 62);
        assert!(boundary[30..].bytes().all(|b| b.is_ascii_hexdigit()));
    }
}
