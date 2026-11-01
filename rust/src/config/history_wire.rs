use std::path::Path;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use super::HistoryDocumentError as Error;

const WINDOWS_EPOCH_SECONDS: i128 = 11_644_473_600;
const TICKS_PER_SECOND: i128 = 10_000_000;

pub(super) fn decode_file_time(ticks: i64) -> Result<SystemTime, Error> {
    if ticks < 0 {
        return Err(Error::InvalidRecord);
    }
    from_nanos((i128::from(ticks) - WINDOWS_EPOCH_SECONDS * TICKS_PER_SECOND) * 100)
}

pub(super) fn encode_file_time(time: SystemTime) -> Result<i64, Error> {
    let nanos = unix_nanos(time);
    // Rounding would change the deduplication identity after a restart.
    if nanos % 100 != 0 {
        return Err(Error::InvalidRecord);
    }
    let ticks = nanos / 100 + WINDOWS_EPOCH_SECONDS * TICKS_PER_SECOND;
    if ticks < 0 {
        return Err(Error::InvalidRecord);
    }
    i64::try_from(ticks).map_err(|_| Error::InvalidRecord)
}

pub(super) fn decode_detected_time(milliseconds: i64) -> Result<SystemTime, Error> {
    from_nanos(i128::from(milliseconds) * 1_000_000)
}

pub(super) fn encode_detected_time(time: SystemTime) -> Result<i64, Error> {
    // Detection time is display data. Match C++ duration_cast's truncation toward zero.
    i64::try_from(unix_nanos(time) / 1_000_000).map_err(|_| Error::InvalidRecord)
}

fn unix_nanos(time: SystemTime) -> i128 {
    match time.duration_since(UNIX_EPOCH) {
        Ok(duration) => duration.as_nanos() as i128,
        Err(error) => -(error.duration().as_nanos() as i128),
    }
}

fn from_nanos(nanos: i128) -> Result<SystemTime, Error> {
    let magnitude = nanos.unsigned_abs();
    let duration = Duration::new(
        u64::try_from(magnitude / 1_000_000_000).map_err(|_| Error::InvalidRecord)?,
        (magnitude % 1_000_000_000) as u32,
    );
    if nanos < 0 {
        UNIX_EPOCH.checked_sub(duration)
    } else {
        UNIX_EPOCH.checked_add(duration)
    }
    .ok_or(Error::InvalidRecord)
}

pub(super) fn encode_path(path: &Path) -> Result<&str, Error> {
    let text = path.to_str().ok_or(Error::InvalidRecord)?;
    #[cfg(windows)]
    {
        use std::path::{Component, Prefix};
        if let Some(Component::Prefix(prefix)) = path.components().next() {
            if matches!(prefix.kind(), Prefix::VerbatimDisk(_)) {
                return Ok(&text[4..]);
            }
        }
    }
    Ok(text)
}

pub(super) fn encode_unc_path(path: &Path) -> Result<Option<String>, Error> {
    #[cfg(windows)]
    {
        use std::path::{Component, Prefix};
        if let Some(Component::Prefix(prefix)) = path.components().next() {
            if matches!(prefix.kind(), Prefix::VerbatimUNC(_, _)) {
                let text = path.to_str().ok_or(Error::InvalidRecord)?;
                if text.len() > 4102 {
                    return Err(Error::InvalidRecord);
                }
                return Ok(Some(format!("\\\\{}", &text[8..])));
            }
        }
    }
    let _ = path;
    Ok(None)
}
