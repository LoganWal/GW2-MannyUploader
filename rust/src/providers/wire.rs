use crate::application::HttpResponse;

pub(super) fn retry_after(response: &HttpResponse) -> Option<u64> {
    let mut matching = response
        .headers
        .iter()
        .filter(|header| header.name().eq_ignore_ascii_case("Retry-After"));
    let bytes = matching.next()?.expose_value();
    if matching.next().is_some() || bytes.is_empty() || !bytes.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let seconds = bytes.iter().try_fold(0u64, |total, &byte| {
        total.checked_mul(10)?.checked_add(u64::from(byte - b'0'))
    })?;
    (1..=900).contains(&seconds).then_some(seconds)
}

pub(super) fn trusted_report_permalink(value: &str) -> bool {
    value.len() <= 2048
        && value
            .bytes()
            .all(|b| (0x21..=0x7e).contains(&b) && !matches!(b, b'@' | b'?' | b'#' | b'\\' | b'"'))
        && [
            "https://dps.report/",
            "https://b.dps.report/",
            "https://wvw.report/",
        ]
        .iter()
        .any(|prefix| {
            value
                .strip_prefix(prefix)
                .is_some_and(|path| !path.is_empty())
        })
}

pub(super) fn percent_encode(value: &str) -> String {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let mut encoded = String::with_capacity(value.len() * 3);
    for byte in value.bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) {
            encoded.push(char::from(byte));
        } else {
            encoded.push('%');
            encoded.push(char::from(HEX[(byte >> 4) as usize]));
            encoded.push(char::from(HEX[(byte & 15) as usize]));
        }
    }
    encoded
}
