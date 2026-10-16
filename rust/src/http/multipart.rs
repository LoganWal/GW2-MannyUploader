use std::collections::VecDeque;
use std::panic::{AssertUnwindSafe, catch_unwind};

use super::{ExactLengthBody, MemoryBodySource};
use crate::application::{
    HttpBodyError, HttpBodyErrorCode, HttpBodySource, MAX_HTTP_REQUEST_BODY_BYTES, RandomSource,
    StopToken, is_http_token_byte,
};

pub const MAX_MULTIPART_BOUNDARY_BYTES: usize = 70;
pub const MAX_MULTIPART_PARTS: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MultipartError {
    EmptyParts,
    TooManyParts,
    InvalidBoundary,
    InvalidPartName,
    InvalidFilename,
    InvalidContentType,
    BodyTooLarge,
    BoundaryGenerationFailed,
    InvalidBody,
}

pub struct MultipartPart {
    pub name: String,
    pub filename: Option<String>,
    pub content_type: Option<String>,
    pub body: Box<dyn HttpBodySource>,
}
pub struct MultipartFormData {
    pub content_type: String,
    pub body: Box<dyn HttpBodySource>,
}
impl MultipartFormData {
    pub fn new(
        parts: Vec<MultipartPart>,
        random: &dyn RandomSource,
    ) -> Result<Self, MultipartError> {
        validate_count(parts.len())?;
        let mut bytes = [0; 16];
        catch_unwind(AssertUnwindSafe(|| random.fill(&mut bytes)))
            .map_err(|_| MultipartError::BoundaryGenerationFailed)?
            .map_err(|_| MultipartError::BoundaryGenerationFailed)?;
        let mut boundary = String::from("------------------------manny-");
        const HEX: &[u8; 16] = b"0123456789abcdef";
        for byte in bytes {
            boundary.push(HEX[(byte >> 4) as usize] as char);
            boundary.push(HEX[(byte & 15) as usize] as char);
        }
        Self::with_boundary(parts, &boundary)
    }
    pub fn with_boundary(
        parts: Vec<MultipartPart>,
        boundary: &str,
    ) -> Result<Self, MultipartError> {
        validate_count(parts.len())?;
        if boundary.is_empty()
            || boundary.len() > MAX_MULTIPART_BOUNDARY_BYTES
            || !boundary.bytes().all(is_http_token_byte)
        {
            return Err(MultipartError::InvalidBoundary);
        }
        let mut segments = VecDeque::new();
        let mut length = 0;
        for (index, part) in parts.into_iter().enumerate() {
            validate_part(&part)?;
            let body = ExactLengthBody::new(part.body).map_err(|e| {
                if e.code == HttpBodyErrorCode::Panicked {
                    MultipartError::InvalidBody
                } else {
                    MultipartError::BodyTooLarge
                }
            })?;
            let mut prefix = format!(
                "{}--{boundary}\r\nContent-Disposition: form-data; name=\"{}\"",
                if index == 0 { "" } else { "\r\n" },
                part.name
            );
            if let Some(filename) = part.filename {
                prefix.push_str(&format!("; filename=\"{filename}\""));
            }
            prefix.push_str("\r\n");
            if let Some(content_type) = part.content_type {
                prefix.push_str("Content-Type: ");
                prefix.push_str(&content_type);
                prefix.push_str("\r\n");
            }
            prefix.push_str("\r\n");
            append(&mut segments, &mut length, memory(prefix)?)?;
            append(&mut segments, &mut length, body)?;
        }
        append(
            &mut segments,
            &mut length,
            memory(format!("\r\n--{boundary}--\r\n"))?,
        )?;
        Ok(Self {
            content_type: format!("multipart/form-data; boundary={boundary}"),
            body: Box::new(MultipartBody {
                segments,
                length,
                failure: None,
            }),
        })
    }
}
fn validate_count(count: usize) -> Result<(), MultipartError> {
    if count == 0 {
        Err(MultipartError::EmptyParts)
    } else if count > MAX_MULTIPART_PARTS {
        Err(MultipartError::TooManyParts)
    } else {
        Ok(())
    }
}
fn validate_part(part: &MultipartPart) -> Result<(), MultipartError> {
    let simple = |b: u8| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-');
    if part.name.is_empty() || part.name.len() > 64 || !part.name.bytes().all(simple) {
        return Err(MultipartError::InvalidPartName);
    }
    if part.filename.as_ref().is_some_and(|name| {
        name.is_empty() || name.len() > 255 || !name.bytes().all(|b| simple(b) || b == b'.')
    }) {
        return Err(MultipartError::InvalidFilename);
    }
    if let Some(value) = &part.content_type {
        let valid = value.len() <= 255
            && value.split_once('/').is_some_and(|(kind, subtype)| {
                !kind.is_empty()
                    && !subtype.is_empty()
                    && kind.bytes().all(is_http_token_byte)
                    && subtype.bytes().all(is_http_token_byte)
            });
        if !valid {
            return Err(MultipartError::InvalidContentType);
        }
    }
    Ok(())
}
fn memory(value: String) -> Result<ExactLengthBody, MultipartError> {
    let source =
        MemoryBodySource::new(value.into_bytes()).map_err(|_| MultipartError::BodyTooLarge)?;
    ExactLengthBody::new(Box::new(source)).map_err(|_| MultipartError::InvalidBody)
}
fn append(
    segments: &mut VecDeque<ExactLengthBody>,
    total: &mut u64,
    source: ExactLengthBody,
) -> Result<(), MultipartError> {
    let length = source.content_length();
    if length > MAX_HTTP_REQUEST_BODY_BYTES - *total {
        return Err(MultipartError::BodyTooLarge);
    }
    *total += length;
    segments.push_back(source);
    Ok(())
}
struct MultipartBody {
    segments: VecDeque<ExactLengthBody>,
    length: u64,
    failure: Option<HttpBodyError>,
}
impl HttpBodySource for MultipartBody {
    fn content_length(&self) -> u64 {
        self.length
    }
    fn read(&mut self, destination: &mut [u8], stop: &StopToken) -> Result<usize, HttpBodyError> {
        if let Some(error) = self.failure {
            return Err(error);
        }
        if stop.is_stop_requested() {
            let error = HttpBodyError::new(HttpBodyErrorCode::Cancelled);
            self.failure = Some(error);
            self.segments.clear();
            return Err(error);
        }
        if destination.is_empty() {
            return Ok(0);
        }
        while let Some(source) = self.segments.front_mut() {
            match source.read(destination, stop) {
                Ok(0) => {
                    self.segments.pop_front();
                }
                Ok(count) => return Ok(count),
                Err(error) => {
                    self.failure = Some(error);
                    self.segments.clear();
                    return Err(error);
                }
            }
        }
        Ok(0)
    }
}
