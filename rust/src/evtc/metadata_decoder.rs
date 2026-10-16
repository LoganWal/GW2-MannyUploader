use crate::application::{MetadataError, StopToken};
use crate::domain::EncounterMetadata;

const AGENT_SIZE: usize = 96;
const SKILL_SIZE: usize = 68;
const EVENT_SIZE: usize = 64;
const MAX_AGENTS: usize = 100_000;
const MAX_SKILLS: usize = 65_535;
const MAX_EVENTS: usize = 10_000_000;

struct Cursor<'a>(&'a [u8]);

impl<'a> Cursor<'a> {
    fn take(&mut self, length: usize) -> Result<&'a [u8], MetadataError> {
        let bytes = self.0.get(..length).ok_or(MetadataError::MalformedLog)?;
        self.0 = &self.0[length..];
        Ok(bytes)
    }

    fn table(&mut self, width: usize, limit: usize) -> Result<&'a [u8], MetadataError> {
        let count = u32_at(self.take(4)?, 0) as usize;
        if count > limit {
            return Err(MetadataError::MalformedLog);
        }
        self.take(
            count
                .checked_mul(width)
                .ok_or(MetadataError::MalformedLog)?,
        )
    }
}

// Callers pass only complete, size-validated wire records.
fn u32_at(record: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(
        record[offset..offset + 4]
            .try_into()
            .expect("validated wire field"),
    )
}

fn u64_at(record: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(
        record[offset..offset + 8]
            .try_into()
            .expect("validated wire field"),
    )
}

fn account(agent: &[u8]) -> Result<String, MetadataError> {
    let mut segments = agent[28..92].split(|byte| *byte == 0);
    segments.next();
    let account = segments.next().ok_or(MetadataError::MalformedLog)?;
    // A following segment proves that the account terminator was inside the fixed field.
    segments.next().ok_or(MetadataError::MalformedLog)?;
    if account.is_empty() {
        return Err(MetadataError::MissingPointOfView);
    }
    Ok(std::str::from_utf8(account)
        .map_err(|_| MetadataError::MalformedLog)?
        .to_owned())
}

/// Decode the frozen revision-1 subset from an already bounded, uncompressed payload.
pub fn decode_metadata(
    payload: &[u8],
    stop: &StopToken,
) -> Result<EncounterMetadata, MetadataError> {
    decode(payload, || stop.is_stop_requested())
}

fn decode(
    payload: &[u8],
    mut cancelled: impl FnMut() -> bool,
) -> Result<EncounterMetadata, MetadataError> {
    let mut check_stop = || {
        if cancelled() {
            Err(MetadataError::Cancelled)
        } else {
            Ok(())
        }
    };
    check_stop()?;
    let mut cursor = Cursor(payload);
    let header = cursor.take(16)?;
    if &header[..4] != b"EVTC" || header[12] != 1 {
        return Err(MetadataError::UnsupportedFormat);
    }
    let boss_id = u16::from_le_bytes([header[13], header[14]]);
    let agents = cursor.table(AGENT_SIZE, MAX_AGENTS)?;
    cursor.table(SKILL_SIZE, MAX_SKILLS)?;
    let events = cursor.0;
    if events.len() % EVENT_SIZE != 0 || events.len() / EVENT_SIZE > MAX_EVENTS {
        return Err(MetadataError::MalformedLog);
    }

    let mut bosses = Vec::new();
    bosses
        .try_reserve_exact(agents.len() / AGENT_SIZE)
        .map_err(|_| MetadataError::ResourceLimit)?;
    for agent in agents.chunks_exact(AGENT_SIZE) {
        check_stop()?;
        let address = u64_at(agent, 0);
        let species = u32_at(agent, 8);
        if address != 0
            && u32_at(agent, 12) == u32::MAX
            && species >> 16 != u16::MAX as u32
            && species as u16 == boss_id
        {
            bosses.push(address);
        }
    }
    bosses.sort_unstable();
    check_stop()?;
    let mut pov = None;
    let mut health = None;
    for event in events.chunks_exact(EVENT_SIZE) {
        check_stop()?;
        let source = u64_at(event, 8);
        match event[56] {
            13 if pov.is_none() && source != 0 => pov = Some(source),
            8 if bosses.binary_search(&source).is_ok() => {
                let value = u64_at(event, 16);
                if value <= 10_000 {
                    health = Some(value as u16);
                }
            }
            _ => {}
        }
    }
    let pov = pov.ok_or(MetadataError::MissingPointOfView)?;
    for agent in agents.chunks_exact(AGENT_SIZE) {
        check_stop()?;
        if u64_at(agent, 0) == pov {
            if u32_at(agent, 12) == u32::MAX {
                return Err(MetadataError::MissingPointOfView);
            }
            return Ok(EncounterMetadata {
                boss_id,
                pov_account: account(agent)?,
                remaining_health_basis_points: health,
            });
        }
    }
    Err(MetadataError::MissingPointOfView)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cancellation_is_checked_during_agent_and_event_scans() {
        let mut bytes = vec![0; 16];
        bytes[..4].copy_from_slice(b"EVTC");
        bytes[12] = 1;
        bytes.extend_from_slice(&4u32.to_le_bytes());
        bytes.resize(bytes.len() + 4 * AGENT_SIZE, 0);
        bytes.extend_from_slice(&0u32.to_le_bytes());
        bytes.resize(bytes.len() + 4 * EVENT_SIZE, 0);
        for stop_at in [2, 4, 7, 9] {
            let mut checks = 0;
            let result = decode(&bytes, || {
                checks += 1;
                checks == stop_at
            });
            assert_eq!(result, Err(MetadataError::Cancelled));
        }
    }
}
