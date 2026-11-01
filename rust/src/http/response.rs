use zeroize::{Zeroize, Zeroizing};

use crate::application::{
    HttpError, HttpErrorCode, HttpHeader, HttpResponse, HttpResponseLimits,
    MAX_HTTP_RESPONSE_BODY_BYTES, MAX_HTTP_RESPONSE_HEADER_BYTES, SecretValue, StopToken,
    is_http_token_byte,
};

/// Receives complete backend header lines and arbitrary body chunks without exposing partial results.
pub struct ResponseCollector {
    limits: HttpResponseLimits,
    header_bytes: usize,
    headers: Vec<HttpHeader>,
    body: Zeroizing<Vec<u8>>,
    status: Option<u16>,
    headers_ended: bool,
    failure: Option<HttpError>,
}
impl ResponseCollector {
    pub fn new(limits: HttpResponseLimits) -> Result<Self, HttpError> {
        if !(1..=MAX_HTTP_RESPONSE_HEADER_BYTES).contains(&limits.max_header_bytes)
            || !(1..=MAX_HTTP_RESPONSE_BODY_BYTES).contains(&limits.max_body_bytes)
        {
            return Err(HttpError::new(HttpErrorCode::InvalidRequest));
        }
        Ok(Self {
            limits,
            header_bytes: 0,
            headers: Vec::new(),
            body: Zeroizing::new(Vec::new()),
            status: None,
            headers_ended: false,
            failure: None,
        })
    }
    fn fail<T>(&mut self, code: HttpErrorCode) -> Result<T, HttpError> {
        let error = *self.failure.get_or_insert(HttpError::new(code));
        self.headers.clear();
        self.body.zeroize();
        Err(error)
    }
    fn check(&mut self, stop: &StopToken) -> Result<(), HttpError> {
        if let Some(error) = self.failure {
            return Err(error);
        }
        if stop.is_stop_requested() {
            return self.fail(HttpErrorCode::Cancelled);
        }
        Ok(())
    }
    pub fn header(&mut self, bytes: &[u8], stop: &StopToken) -> Result<(), HttpError> {
        self.check(stop)?;
        if bytes.len() > self.limits.max_header_bytes - self.header_bytes {
            return self.fail(HttpErrorCode::ResponseTooLarge);
        }
        self.header_bytes += bytes.len();
        let Some(line) = bytes.strip_suffix(b"\n") else {
            return self.fail(HttpErrorCode::ProtocolError);
        };
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        if !line.iter().all(|&b| b == b'\t' || b >= 0x20 && b != 0x7f) {
            return self.fail(HttpErrorCode::ProtocolError);
        }
        if line.starts_with(b"HTTP/") {
            // Interim responses replace their headers, but still consume the cumulative byte budget.
            if self.status.is_some_and(|s| s >= 200) || self.status.is_some() && !self.headers_ended
            {
                return self.fail(HttpErrorCode::ProtocolError);
            }
            let Some((version, rest)) = line.split_once_byte(b' ') else {
                return self.fail(HttpErrorCode::ProtocolError);
            };
            if ![b"HTTP/1.0".as_slice(), b"HTTP/1.1", b"HTTP/2", b"HTTP/3"].contains(&version)
                || rest.len() < 3
                || !rest[..3].iter().all(u8::is_ascii_digit)
                || rest.len() > 3 && rest[3] != b' '
            {
                return self.fail(HttpErrorCode::ProtocolError);
            }
            let status = u16::from(rest[0] - b'0') * 100
                + u16::from(rest[1] - b'0') * 10
                + u16::from(rest[2] - b'0');
            if !(100..=599).contains(&status) || status == 101 {
                return self.fail(HttpErrorCode::ProtocolError);
            }
            self.status = Some(status);
            self.headers_ended = false;
            self.headers.clear();
            return Ok(());
        }
        if self.status.is_none() {
            return self.fail(HttpErrorCode::ProtocolError);
        }
        if line.is_empty() {
            self.headers_ended = true;
            return Ok(());
        }
        if self.headers_ended && self.status.is_some_and(|s| s < 200) {
            return self.fail(HttpErrorCode::ProtocolError);
        }
        let Some((name, value)) = line.split_once_byte(b':') else {
            return self.fail(HttpErrorCode::ProtocolError);
        };
        if name.is_empty() || !name.iter().copied().all(is_http_token_byte) {
            return self.fail(HttpErrorCode::ProtocolError);
        }
        let value = value.trim_ascii_start().trim_ascii_end();
        let name = String::from_utf8(name.to_vec())
            .map_err(|_| HttpError::new(HttpErrorCode::Internal))?;
        self.headers.push(HttpHeader::response(
            name,
            SecretValue::from_bytes(value.to_vec()),
        ));
        self.check(stop)
    }
    pub fn body(&mut self, bytes: &[u8], stop: &StopToken) -> Result<(), HttpError> {
        self.check(stop)?;
        if !self.headers_ended || !self.status.is_some_and(|s| s >= 200) {
            return self.fail(HttpErrorCode::ProtocolError);
        }
        if bytes.len() > self.limits.max_body_bytes - self.body.len() {
            return self.fail(HttpErrorCode::ResponseTooLarge);
        }
        if !bytes.is_empty() && self.body.capacity() == 0 {
            // Reserve the bounded response once, before secrets arrive, to avoid plaintext reallocations.
            if self
                .body
                .try_reserve_exact(self.limits.max_body_bytes)
                .is_err()
            {
                return self.fail(HttpErrorCode::Internal);
            }
        }
        self.body.extend_from_slice(bytes);
        self.check(stop)
    }
    pub fn finish(mut self, status_code: u16, stop: &StopToken) -> Result<HttpResponse, HttpError> {
        self.check(stop)?;
        if !self.headers_ended || status_code < 200 || self.status != Some(status_code) {
            return self.fail(HttpErrorCode::ProtocolError);
        }
        Ok(HttpResponse {
            status_code,
            headers: self.headers,
            body: SecretValue::from_bytes(std::mem::take(&mut *self.body)),
        })
    }
}

trait SplitByte {
    fn split_once_byte(&self, delimiter: u8) -> Option<(&[u8], &[u8])>;
}
impl SplitByte for [u8] {
    fn split_once_byte(&self, delimiter: u8) -> Option<(&[u8], &[u8])> {
        let index = self.iter().position(|&b| b == delimiter)?;
        Some((&self[..index], &self[index + 1..]))
    }
}
