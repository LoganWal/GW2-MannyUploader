use std::fs::{self, File, Metadata};
use std::io::{ErrorKind, Read};
use std::panic::{AssertUnwindSafe, catch_unwind};

use crate::application::{
    HttpBodyError, HttpBodyErrorCode, HttpBodySource, MAX_HTTP_REQUEST_BODY_BYTES, SecretValue,
    StopToken,
};
use crate::domain::LogFileIdentity;

fn error(code: HttpBodyErrorCode) -> HttpBodyError {
    HttpBodyError::new(code)
}

pub struct MemoryBodySource {
    bytes: SecretValue,
    offset: usize,
}
impl MemoryBodySource {
    pub fn new(bytes: Vec<u8>) -> Result<Self, HttpBodyError> {
        Self::from_secret(SecretValue::from_bytes(bytes))
    }
    /// Moves protected data directly into the request without making another plaintext copy.
    pub fn from_secret(bytes: SecretValue) -> Result<Self, HttpBodyError> {
        if bytes.len() as u64 > MAX_HTTP_REQUEST_BODY_BYTES {
            return Err(error(HttpBodyErrorCode::ReadFailed));
        }
        Ok(Self { bytes, offset: 0 })
    }
}
impl HttpBodySource for MemoryBodySource {
    fn content_length(&self) -> u64 {
        self.bytes.len() as u64
    }
    fn read(&mut self, destination: &mut [u8], stop: &StopToken) -> Result<usize, HttpBodyError> {
        if stop.is_stop_requested() {
            return Err(error(HttpBodyErrorCode::Cancelled));
        }
        let count = destination.len().min(self.bytes.len() - self.offset);
        destination[..count]
            .copy_from_slice(&self.bytes.expose()[self.offset..self.offset + count]);
        self.offset += count;
        Ok(count)
    }
}

pub struct FileBodySource {
    file: File,
    identity: LogFileIdentity,
    offset: u64,
    failure: Option<HttpBodyError>,
}
impl FileBodySource {
    pub fn new(identity: LogFileIdentity) -> Result<Self, HttpBodyError> {
        if identity.size > MAX_HTTP_REQUEST_BODY_BYTES {
            return Err(error(HttpBodyErrorCode::ReadFailed));
        }
        verify_path(&identity)?;
        let file = File::open(&identity.canonical_path)
            .map_err(|_| error(HttpBodyErrorCode::SourceUnavailable))?;
        verify_metadata(
            &file
                .metadata()
                .map_err(|_| error(HttpBodyErrorCode::SourceUnavailable))?,
            &identity,
        )?;
        Ok(Self {
            file,
            identity,
            offset: 0,
            failure: None,
        })
    }
    fn read_inner(
        &mut self,
        destination: &mut [u8],
        stop: &StopToken,
    ) -> Result<usize, HttpBodyError> {
        if stop.is_stop_requested() {
            return Err(error(HttpBodyErrorCode::Cancelled));
        }
        if destination.is_empty() {
            return Ok(0);
        }
        let remaining = self.identity.size - self.offset;
        let count = remaining.min(destination.len() as u64) as usize;
        if count != 0 {
            let read = loop {
                match self.file.read(&mut destination[..count]) {
                    Err(e) if e.kind() == ErrorKind::Interrupted => {
                        if stop.is_stop_requested() {
                            return Err(error(HttpBodyErrorCode::Cancelled));
                        }
                    }
                    result => break result.map_err(|_| error(HttpBodyErrorCode::ReadFailed))?,
                }
            };
            if read == 0 {
                return Err(error(HttpBodyErrorCode::SourceChanged));
            }
            self.offset += read as u64;
            if self.offset < self.identity.size {
                return Ok(read);
            }
            self.verify_final()?;
            if stop.is_stop_requested() {
                return Err(error(HttpBodyErrorCode::Cancelled));
            }
            return Ok(read);
        }
        self.verify_final()?;
        Ok(0)
    }
    fn verify_final(&self) -> Result<(), HttpBodyError> {
        verify_metadata(
            &self
                .file
                .metadata()
                .map_err(|_| error(HttpBodyErrorCode::SourceUnavailable))?,
            &self.identity,
        )?;
        verify_path(&self.identity)
    }
}
impl HttpBodySource for FileBodySource {
    fn content_length(&self) -> u64 {
        self.identity.size
    }
    fn read(&mut self, destination: &mut [u8], stop: &StopToken) -> Result<usize, HttpBodyError> {
        if let Some(failure) = self.failure {
            return Err(failure);
        }
        let result = self.read_inner(destination, stop);
        if let Err(failure) = result {
            self.failure = Some(failure);
        }
        result
    }
}
fn verify_path(identity: &LogFileIdentity) -> Result<(), HttpBodyError> {
    let metadata = fs::symlink_metadata(&identity.canonical_path)
        .map_err(|_| error(HttpBodyErrorCode::SourceUnavailable))?;
    if metadata.file_type().is_symlink() {
        return Err(error(HttpBodyErrorCode::SourceUnavailable));
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        if metadata.file_attributes() & 0x400 != 0 {
            return Err(error(HttpBodyErrorCode::SourceUnavailable));
        }
    }
    verify_metadata(&metadata, identity)?;
    let actual = LogFileIdentity {
        canonical_path: fs::canonicalize(&identity.canonical_path)
            .map_err(|_| error(HttpBodyErrorCode::SourceUnavailable))?,
        size: identity.size,
        last_write_time: identity.last_write_time,
    };
    if actual != *identity {
        return Err(error(HttpBodyErrorCode::SourceChanged));
    }
    Ok(())
}
fn verify_metadata(metadata: &Metadata, identity: &LogFileIdentity) -> Result<(), HttpBodyError> {
    if !metadata.is_file() {
        return Err(error(HttpBodyErrorCode::SourceUnavailable));
    }
    let modified = metadata
        .modified()
        .map_err(|_| error(HttpBodyErrorCode::SourceUnavailable))?;
    if metadata.len() != identity.size || modified != identity.last_write_time {
        return Err(error(HttpBodyErrorCode::SourceChanged));
    }
    Ok(())
}

/// Captures one declared length and verifies EOF before returning the final bytes to the adapter.
pub struct ExactLengthBody {
    source: Box<dyn HttpBodySource>,
    length: u64,
    remaining: u64,
    eof_checked: bool,
    failure: Option<HttpBodyError>,
}
impl ExactLengthBody {
    pub fn new(source: Box<dyn HttpBodySource>) -> Result<Self, HttpBodyError> {
        let length = catch_unwind(AssertUnwindSafe(|| source.content_length()))
            .map_err(|_| error(HttpBodyErrorCode::Panicked))?;
        if length > MAX_HTTP_REQUEST_BODY_BYTES {
            return Err(error(HttpBodyErrorCode::ReadFailed));
        }
        Ok(Self {
            source,
            length,
            remaining: length,
            eof_checked: false,
            failure: None,
        })
    }
    pub const fn is_complete(&self) -> bool {
        self.eof_checked && self.failure.is_none()
    }
    fn read_inner(
        &mut self,
        destination: &mut [u8],
        stop: &StopToken,
    ) -> Result<usize, HttpBodyError> {
        if stop.is_stop_requested() {
            return Err(error(HttpBodyErrorCode::Cancelled));
        }
        if destination.is_empty() || self.eof_checked {
            return Ok(0);
        }
        let capacity = self.remaining.min(destination.len() as u64) as usize;
        let read = if capacity == 0 {
            0
        } else {
            let read = self.source.read(&mut destination[..capacity], stop)?;
            if read == 0 || read > capacity {
                return Err(error(HttpBodyErrorCode::ReadFailed));
            }
            self.remaining -= read as u64;
            read
        };
        if stop.is_stop_requested() {
            return Err(error(HttpBodyErrorCode::Cancelled));
        }
        if self.remaining == 0 {
            let mut extra = SecretValue::from_bytes(vec![0; 1]);
            if self.source.read(extra.expose_mut(), stop)? != 0 {
                return Err(error(HttpBodyErrorCode::ReadFailed));
            }
            self.eof_checked = true;
        }
        if stop.is_stop_requested() {
            return Err(error(HttpBodyErrorCode::Cancelled));
        }
        Ok(read)
    }
}
impl HttpBodySource for ExactLengthBody {
    fn content_length(&self) -> u64 {
        self.length
    }
    fn read(&mut self, destination: &mut [u8], stop: &StopToken) -> Result<usize, HttpBodyError> {
        if let Some(failure) = self.failure {
            return Err(failure);
        }
        let result = catch_unwind(AssertUnwindSafe(|| self.read_inner(destination, stop)))
            .unwrap_or(Err(error(HttpBodyErrorCode::Panicked)));
        if let Err(failure) = result {
            self.failure = Some(failure);
        }
        result
    }
}
