use std::time::{Duration, Instant, SystemTime};

const DAY: Duration = Duration::from_secs(24 * 60 * 60);
const REFRESH_INTERVAL: Duration = Duration::from_secs(60);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LogSelectionMode {
    New,
    Last24Hours,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelectionError {
    TimeOutOfRange,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SessionLogSelection {
    started: SystemTime,
    mode: LogSelectionMode,
    discovery_cutoff: SystemTime,
    refreshed: Instant,
}

impl SessionLogSelection {
    pub fn new(started: SystemTime, steady: Instant) -> Self {
        Self {
            started,
            mode: LogSelectionMode::New,
            discovery_cutoff: started,
            refreshed: steady,
        }
    }

    pub fn mode(&self) -> LogSelectionMode {
        self.mode
    }

    pub fn discovery_cutoff(&self) -> SystemTime {
        self.discovery_cutoff
    }

    pub fn display_cutoff(&self, now: SystemTime) -> Result<SystemTime, SelectionError> {
        self.cutoff(self.mode, now)
    }

    pub fn includes(
        &self,
        last_write: SystemTime,
        now: SystemTime,
    ) -> Result<bool, SelectionError> {
        Ok(last_write >= self.display_cutoff(now)?)
    }

    /// Apply the proposed source cutoff successfully before replacing the current selection.
    /// An unsuccessful reconfiguration can simply discard the proposal without losing a retry.
    pub fn propose(
        &self,
        mode: LogSelectionMode,
        now: SystemTime,
        steady: Instant,
    ) -> Result<Option<Self>, SelectionError> {
        if mode == self.mode
            && (mode == LogSelectionMode::New
                || steady.saturating_duration_since(self.refreshed) < REFRESH_INTERVAL)
        {
            return Ok(None);
        }
        let cutoff = self.cutoff(mode, now)?;
        if mode == self.mode && cutoff == self.discovery_cutoff {
            return Ok(None);
        }
        Ok(Some(Self {
            mode,
            discovery_cutoff: cutoff,
            refreshed: steady,
            ..*self
        }))
    }

    fn cutoff(
        &self,
        mode: LogSelectionMode,
        now: SystemTime,
    ) -> Result<SystemTime, SelectionError> {
        match mode {
            LogSelectionMode::New => Ok(self.started),
            LogSelectionMode::Last24Hours => {
                now.checked_sub(DAY).ok_or(SelectionError::TimeOutOfRange)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn new_window_is_fixed_and_rolling_display_moves_before_discovery_refresh() {
        let start = SystemTime::UNIX_EPOCH + DAY * 3;
        let steady = Instant::now();
        let new = SessionLogSelection::new(start, steady);
        assert!(!new.includes(start - Duration::from_secs(1), start).unwrap());
        assert!(new.includes(start, start + DAY).unwrap());
        assert!(
            new.propose(LogSelectionMode::New, start + DAY, steady + DAY)
                .unwrap()
                .is_none()
        );
        let rolling = new
            .propose(LogSelectionMode::Last24Hours, start, steady)
            .unwrap()
            .unwrap();
        assert_eq!(rolling.discovery_cutoff(), start - DAY);
        assert!(rolling.includes(start - DAY, start).unwrap());
        let later = start + Duration::from_secs(59);
        assert_eq!(rolling.display_cutoff(later), Ok(later - DAY));
        assert!(!rolling.includes(start - DAY, later).unwrap());
        assert!(
            rolling
                .propose(
                    LogSelectionMode::Last24Hours,
                    later,
                    steady + Duration::from_secs(59)
                )
                .unwrap()
                .is_none()
        );
        let next = rolling
            .propose(
                LogSelectionMode::Last24Hours,
                start + REFRESH_INTERVAL,
                steady + REFRESH_INTERVAL,
            )
            .unwrap()
            .unwrap();
        assert_eq!(next.discovery_cutoff(), start + REFRESH_INTERVAL - DAY);
        let switched = next
            .propose(LogSelectionMode::New, start + DAY, steady + DAY)
            .unwrap()
            .unwrap();
        assert_eq!(switched.discovery_cutoff(), start);
    }

    #[test]
    fn failed_proposals_do_not_consume_refresh_and_monotonic_time_controls_cadence() {
        let now = SystemTime::UNIX_EPOCH + DAY * 3;
        let steady = Instant::now();
        let initial = SessionLogSelection::new(now, steady);
        let proposal = initial
            .propose(LogSelectionMode::Last24Hours, now, steady)
            .unwrap();
        assert_eq!(initial.mode(), LogSelectionMode::New);
        assert_eq!(
            initial
                .propose(LogSelectionMode::Last24Hours, now, steady)
                .unwrap(),
            proposal
        );
        let rolling = proposal.unwrap();
        assert!(
            rolling
                .propose(LogSelectionMode::Last24Hours, now + DAY, steady)
                .unwrap()
                .is_none()
        );
        let backwards = rolling
            .propose(
                LogSelectionMode::Last24Hours,
                now - Duration::from_secs(300),
                steady + REFRESH_INTERVAL,
            )
            .unwrap()
            .unwrap();
        assert_eq!(
            backwards.discovery_cutoff(),
            now - DAY - Duration::from_secs(300)
        );
    }
}
