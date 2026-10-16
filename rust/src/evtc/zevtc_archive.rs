use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};

use flate2::bufread::DeflateDecoder;

use super::decode_metadata;
use crate::application::{MetadataError, StopToken};
use crate::domain::{EncounterMetadata, LogFileIdentity};

type Result<T> = std::result::Result<T, MetadataError>;
const INVALID: MetadataError = MetadataError::InvalidArchive;
const LIMIT: MetadataError = MetadataError::ResourceLimit;
const CHUNK: usize = 64 * 1024;

#[derive(Clone, Copy, Debug)]
pub struct ArchiveLimits {
    pub archive_bytes: u64,
    pub uncompressed_bytes: u64,
    pub entries: u64,
    pub filename_bytes: usize,
    pub compression_ratio: u32,
}

impl Default for ArchiveLimits {
    fn default() -> Self {
        Self {
            archive_bytes: 256 * 1024 * 1024,
            uncompressed_bytes: 768 * 1024 * 1024,
            entries: 16,
            filename_bytes: 1024,
            compression_ratio: 200,
        }
    }
}

pub struct ZevtcArchiveReader {
    limits: ArchiveLimits,
}

impl ZevtcArchiveReader {
    pub fn new(limits: ArchiveLimits) -> Result<Self> {
        if limits.archive_bytes == 0
            || limits.uncompressed_bytes == 0
            || limits.entries == 0
            || limits.filename_bytes == 0
            || limits.compression_ratio == 0
        {
            return Err(LIMIT);
        }
        Ok(Self { limits })
    }

    pub fn parse(&self, identity: &LogFileIdentity, stop: &StopToken) -> Result<EncounterMetadata> {
        decode_metadata(&self.extract(identity, stop)?, stop)
    }

    pub fn extract(&self, identity: &LogFileIdentity, stop: &StopToken) -> Result<Vec<u8>> {
        check_stop(stop)?;
        let mut file =
            File::open(&identity.canonical_path).map_err(|_| MetadataError::FileUnavailable)?;
        let metadata = file
            .metadata()
            .map_err(|_| MetadataError::FileUnavailable)?;
        if !metadata.is_file() || metadata.len() != identity.size {
            return Err(MetadataError::FileUnavailable);
        }
        let result = self.extract_from(&mut file, identity.size, stop)?;
        let after = file
            .metadata()
            .map_err(|_| MetadataError::FileUnavailable)?;
        if after.len() != metadata.len() || after.modified().ok() != metadata.modified().ok() {
            return Err(MetadataError::FileUnavailable);
        }
        Ok(result)
    }

    /// Read a bounded archive through a seekable source. No entry is written to disk.
    pub fn extract_from(
        &self,
        reader: &mut (impl Read + Seek),
        size: u64,
        stop: &StopToken,
    ) -> Result<Vec<u8>> {
        check_stop(stop)?;
        if size > self.limits.archive_bytes {
            return Err(LIMIT);
        }
        if reader.seek(SeekFrom::End(0)).map_err(|_| INVALID)? != size {
            return Err(INVALID);
        }
        let directory = directory(reader, size, self.limits)?;
        check_stop(stop)?;
        let bytes = read_bytes(reader, directory.offset, directory.size)?;
        let mut entry = select_entry(&bytes, directory.count, self.limits, stop)?;
        if entry.flags & !0x080e != 0 || !matches!(entry.method, 0 | 8) || entry.uncompressed == 0 {
            return Err(INVALID);
        }
        if entry.uncompressed > self.limits.uncompressed_bytes
            || u128::from(entry.uncompressed)
                > u128::from(entry.compressed) * u128::from(self.limits.compression_ratio)
        {
            return Err(LIMIT);
        }
        let data_start = validate_local(reader, &mut entry, directory.offset)?;
        let data_end = data_start.checked_add(entry.compressed).ok_or(INVALID)?;
        if data_end > directory.offset {
            return Err(INVALID);
        }
        if entry.flags & 8 != 0 {
            validate_descriptor(reader, data_end, directory.offset, &entry)?;
        }
        reader
            .seek(SeekFrom::Start(data_start))
            .map_err(|_| INVALID)?;
        let compressed = reader.take(entry.compressed);
        let mut output = Vec::new();
        output
            .try_reserve_exact(usize::try_from(entry.uncompressed).map_err(|_| LIMIT)?)
            .map_err(|_| LIMIT)?;
        let mut crc = crc32fast::Hasher::new();
        match entry.method {
            0 => {
                if entry.compressed != entry.uncompressed {
                    return Err(INVALID);
                }
                read_payload(compressed, &mut output, entry.uncompressed, &mut crc, stop)?;
            }
            8 => {
                let mut inflater = DeflateDecoder::new(BufReader::with_capacity(CHUNK, compressed));
                read_payload(
                    &mut inflater,
                    &mut output,
                    entry.uncompressed,
                    &mut crc,
                    stop,
                )?;
                if inflater.total_in() != entry.compressed {
                    return Err(INVALID);
                }
            }
            _ => return Err(INVALID),
        }
        if output.len() as u64 != entry.uncompressed || crc.finalize() != entry.crc {
            return Err(INVALID);
        }
        check_stop(stop)?;
        Ok(output)
    }
}

fn check_stop(stop: &StopToken) -> Result<()> {
    if stop.is_stop_requested() {
        Err(MetadataError::Cancelled)
    } else {
        Ok(())
    }
}

fn read_payload(
    mut reader: impl Read,
    output: &mut Vec<u8>,
    expected: u64,
    crc: &mut crc32fast::Hasher,
    stop: &StopToken,
) -> Result<()> {
    let mut chunk = [0; CHUNK];
    loop {
        check_stop(stop)?;
        let count = reader.read(&mut chunk).map_err(|_| INVALID)?;
        if count == 0 {
            return Ok(());
        }
        if output.len() as u64 + count as u64 > expected {
            return Err(INVALID);
        }
        crc.update(&chunk[..count]);
        output.extend_from_slice(&chunk[..count]);
    }
}

fn field<const N: usize>(bytes: &[u8], offset: usize) -> Result<[u8; N]> {
    bytes
        .get(offset..offset.checked_add(N).ok_or(INVALID)?)
        .ok_or(INVALID)?
        .try_into()
        .map_err(|_| INVALID)
}
fn u16_at(bytes: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(field(bytes, offset)?))
}
fn u32_at(bytes: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(field(bytes, offset)?))
}
fn u64_at(bytes: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(field(bytes, offset)?))
}

fn read_at<const N: usize>(reader: &mut (impl Read + Seek), offset: u64) -> Result<[u8; N]> {
    reader.seek(SeekFrom::Start(offset)).map_err(|_| INVALID)?;
    let mut bytes = [0; N];
    reader.read_exact(&mut bytes).map_err(|_| INVALID)?;
    Ok(bytes)
}

fn read_bytes(reader: &mut (impl Read + Seek), offset: u64, size: u64) -> Result<Vec<u8>> {
    let size = usize::try_from(size).map_err(|_| LIMIT)?;
    let mut bytes = Vec::new();
    bytes.try_reserve_exact(size).map_err(|_| LIMIT)?;
    bytes.resize(size, 0);
    reader.seek(SeekFrom::Start(offset)).map_err(|_| INVALID)?;
    reader.read_exact(&mut bytes).map_err(|_| INVALID)?;
    Ok(bytes)
}

struct Directory {
    offset: u64,
    size: u64,
    count: u64,
}

fn directory(
    reader: &mut (impl Read + Seek),
    size: u64,
    limits: ArchiveLimits,
) -> Result<Directory> {
    if size < 22 {
        return Err(INVALID);
    }
    let tail_size = size.min(22 + u16::MAX as u64);
    let tail = read_bytes(reader, size - tail_size, tail_size)?;
    let end = (0..=tail.len() - 22)
        .rev()
        .find(|&offset| {
            tail[offset..offset + 4] == *b"PK\x05\x06"
                && u16_at(&tail, offset + 20)
                    .is_ok_and(|comment| offset + 22 + comment as usize == tail.len())
        })
        .ok_or(INVALID)?;
    let eocd = &tail[end..];
    if u16_at(eocd, 4)? != 0 || u16_at(eocd, 6)? != 0 || u16_at(eocd, 8)? != u16_at(eocd, 10)? {
        return Err(INVALID);
    }
    let end_offset = size - tail_size + end as u64;
    let mut directory_end = end_offset;
    let mut result = Directory {
        offset: u32_at(eocd, 16)? as u64,
        size: u32_at(eocd, 12)? as u64,
        count: u16_at(eocd, 10)? as u64,
    };
    let zip64_required = result.offset == u32::MAX as u64
        || result.size == u32::MAX as u64
        || result.count == u16::MAX as u64;
    let locator = if end_offset >= 20 {
        Some(read_at::<20>(reader, end_offset - 20)?)
    } else {
        None
    };
    if let Some(locator) = locator.filter(|bytes| bytes[..4] == *b"PK\x06\x07") {
        if u32_at(&locator, 4)? != 0 || u32_at(&locator, 16)? != 1 {
            return Err(INVALID);
        }
        let offset = u64_at(&locator, 8)?;
        let record = read_at::<56>(reader, offset)?;
        let record_size = u64_at(&record, 4)?;
        if record[..4] != *b"PK\x06\x06"
            || record_size < 44
            || offset
                .checked_add(12)
                .and_then(|value| value.checked_add(record_size))
                != Some(end_offset - 20)
            || u32_at(&record, 16)? != 0
            || u32_at(&record, 20)? != 0
            || u64_at(&record, 24)? != u64_at(&record, 32)?
        {
            return Err(INVALID);
        }
        let extended = Directory {
            offset: u64_at(&record, 48)?,
            size: u64_at(&record, 40)?,
            count: u64_at(&record, 32)?,
        };
        for (short, long, sentinel) in [
            (result.offset, extended.offset, u32::MAX as u64),
            (result.size, extended.size, u32::MAX as u64),
            (result.count, extended.count, u16::MAX as u64),
        ] {
            if short != sentinel && short != long {
                return Err(INVALID);
            }
        }
        result = extended;
        directory_end = offset;
    } else if zip64_required {
        return Err(INVALID);
    }
    if result.count > limits.entries {
        return Err(LIMIT);
    }
    if result.offset.checked_add(result.size) != Some(directory_end) {
        return Err(INVALID);
    }
    // Every entry has bounded fixed fields, filename, extra data, and comment. Check before allocation.
    let per_entry = 46u64
        .checked_add(limits.filename_bytes as u64)
        .and_then(|n| n.checked_add(2 * u16::MAX as u64))
        .ok_or(LIMIT)?;
    if result.size > result.count.checked_mul(per_entry).ok_or(LIMIT)? {
        return Err(LIMIT);
    }
    Ok(result)
}

struct Entry {
    name: Vec<u8>,
    flags: u16,
    method: u16,
    crc: u32,
    compressed: u64,
    uncompressed: u64,
    offset: u64,
    zip64_sizes: bool,
}

fn zip64_extra(extra: &[u8]) -> Result<Option<&[u8]>> {
    let mut rest = extra;
    let mut found = None;
    while !rest.is_empty() {
        let tag = u16_at(rest, 0)?;
        let length = u16_at(rest, 2)? as usize;
        let value = rest.get(4..4 + length).ok_or(INVALID)?;
        if tag == 1 {
            if found.is_some() {
                return Err(INVALID);
            }
            found = Some(value);
        }
        rest = &rest[4 + length..];
    }
    Ok(found)
}

fn extended(value: u32, extra: &mut &[u8]) -> Result<u64> {
    if value != u32::MAX {
        return Ok(value as u64);
    }
    let value = u64_at(extra, 0)?;
    *extra = &extra[8..];
    Ok(value)
}

fn select_entry(
    mut bytes: &[u8],
    count: u64,
    limits: ArchiveLimits,
    stop: &StopToken,
) -> Result<Entry> {
    let mut selected = None;
    let mut fallback = None;
    let mut files = 0;
    for _ in 0..count {
        check_stop(stop)?;
        if field::<4>(bytes, 0)? != *b"PK\x01\x02" {
            return Err(INVALID);
        }
        let name_len = u16_at(bytes, 28)? as usize;
        let extra_len = u16_at(bytes, 30)? as usize;
        let comment_len = u16_at(bytes, 32)? as usize;
        if name_len > limits.filename_bytes {
            return Err(LIMIT);
        }
        let name = bytes.get(46..46 + name_len).ok_or(INVALID)?;
        let extra = bytes
            .get(46 + name_len..46 + name_len + extra_len)
            .ok_or(INVALID)?;
        let mut zip64 = zip64_extra(extra)?.unwrap_or_default();
        let uncompressed = extended(u32_at(bytes, 24)?, &mut zip64)?;
        let compressed = extended(u32_at(bytes, 20)?, &mut zip64)?;
        let offset = extended(u32_at(bytes, 42)?, &mut zip64)?;
        let disk = u16_at(bytes, 34)?;
        if (disk == u16::MAX && u32_at(zip64, 0)? != 0) || (disk != u16::MAX && disk != 0) {
            return Err(INVALID);
        }
        let is_directory = name.ends_with(b"/") || u32_at(bytes, 38)? & 0x10 != 0;
        if !is_directory {
            files += 1;
            let evtc = name.len() >= 5 && name[name.len() - 5..].eq_ignore_ascii_case(b".evtc");
            let leaf = name
                .rsplit(|byte| matches!(byte, b'/' | b'\\'))
                .next()
                .unwrap_or_default();
            if evtc || (!leaf.is_empty() && !leaf.contains(&b'.')) {
                let entry = Entry {
                    name: name.to_vec(),
                    flags: u16_at(bytes, 8)?,
                    method: u16_at(bytes, 10)?,
                    crc: u32_at(bytes, 16)?,
                    compressed,
                    uncompressed,
                    offset,
                    zip64_sizes: u32_at(bytes, 24)? == u32::MAX || u32_at(bytes, 20)? == u32::MAX,
                };
                if evtc {
                    if selected.is_some() {
                        return Err(INVALID);
                    }
                    selected = Some(entry);
                } else {
                    fallback = Some(entry);
                }
            }
        }
        bytes = bytes
            .get(46 + name_len + extra_len + comment_len..)
            .ok_or(INVALID)?;
    }
    if !bytes.is_empty() {
        return Err(INVALID);
    }
    selected
        .or(if files == 1 { fallback } else { None })
        .ok_or(INVALID)
}

fn validate_local(
    reader: &mut (impl Read + Seek),
    entry: &mut Entry,
    directory_start: u64,
) -> Result<u64> {
    let header = read_at::<30>(reader, entry.offset)?;
    if header[..4] != *b"PK\x03\x04"
        || u16_at(&header, 6)? != entry.flags
        || u16_at(&header, 8)? != entry.method
    {
        return Err(INVALID);
    }
    let name_length = u16_at(&header, 26)? as usize;
    let extra_length = u16_at(&header, 28)? as usize;
    if name_length != entry.name.len() {
        return Err(INVALID);
    }
    let variable_start = entry.offset.checked_add(30).ok_or(INVALID)?;
    let data_start = variable_start
        .checked_add((name_length + extra_length) as u64)
        .ok_or(INVALID)?;
    if data_start > directory_start {
        return Err(INVALID);
    }
    let variable = read_bytes(reader, variable_start, (name_length + extra_length) as u64)?;
    if variable[..name_length] != entry.name {
        return Err(INVALID);
    }
    let mut extra = zip64_extra(&variable[name_length..])?.unwrap_or_default();
    // Streaming writers may use ZIP64 locally even when final central sizes fit in 32 bits.
    entry.zip64_sizes |= u32_at(&header, 22)? == u32::MAX || u32_at(&header, 18)? == u32::MAX;
    let uncompressed = extended(u32_at(&header, 22)?, &mut extra)?;
    let compressed = extended(u32_at(&header, 18)?, &mut extra)?;
    let crc = u32_at(&header, 14)?;
    if entry.flags & 8 == 0 {
        if (crc, compressed, uncompressed) != (entry.crc, entry.compressed, entry.uncompressed) {
            return Err(INVALID);
        }
    } else if (crc != 0 && crc != entry.crc)
        || (compressed != 0 && compressed != entry.compressed)
        || (uncompressed != 0 && uncompressed != entry.uncompressed)
    {
        return Err(INVALID);
    }
    Ok(data_start)
}

fn validate_descriptor(
    reader: &mut (impl Read + Seek),
    offset: u64,
    bound: u64,
    entry: &Entry,
) -> Result<()> {
    let width = if entry.zip64_sizes { 20 } else { 12 };
    let available = bound.checked_sub(offset).ok_or(INVALID)?;
    if available < width {
        return Err(INVALID);
    }
    let bytes = read_bytes(reader, offset, available.min(width + 4))?;
    for start in [0, 4] {
        if start == 4 && u32_at(&bytes, 0)? != 0x08074b50 {
            continue;
        }
        let Some(bytes) = bytes.get(start..start + width as usize) else {
            continue;
        };
        let (compressed, uncompressed) = if entry.zip64_sizes {
            (u64_at(bytes, 4)?, u64_at(bytes, 12)?)
        } else {
            (u32_at(bytes, 4)? as u64, u32_at(bytes, 8)? as u64)
        };
        if (u32_at(bytes, 0)?, compressed, uncompressed)
            == (entry.crc, entry.compressed, entry.uncompressed)
        {
            return Ok(());
        }
    }
    Err(INVALID)
}
