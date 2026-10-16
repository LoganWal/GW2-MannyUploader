use super::{DonBotAggregateCandidate, DonBotAggregateSelection};
use crate::{application::*, domain::*};
use std::{collections::HashSet, time::SystemTime};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProviderCell {
    pub state: ProviderState,
    pub status: &'static str,
    pub detail: String,
    pub retry_available: bool,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecentLogRow {
    pub id: UploadJobId,
    pub detected_at: String,
    pub encounter: String,
    pub encounter_success: Option<bool>,
    pub dps_report_url: Option<String>,
    pub donbot_fight_log_id: Option<u64>,
    pub wingman_report_available: bool,
    pub directory_available: bool,
    pub reupload_available: bool,
    pub rechat_available: bool,
    pub providers: [ProviderCell; 4],
}
impl RecentLogRow {
    pub fn aggregate_candidate(&self) -> DonBotAggregateCandidate {
        DonBotAggregateCandidate {
            job_id: self.id,
            eligible: self.donbot_fight_log_id.is_some(),
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecentLogsModel {
    pub retained_job_ids: Vec<UploadJobId>,
    pub rows: Vec<RecentLogRow>,
    pub enabled_providers: ProviderSelection,
    pub dps_report_clipboard_text: String,
    pub dps_report_url_count: usize,
    pub donbot_aggregate_url: String,
    pub donbot_fight_count: usize,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecentLogsModelError {
    TooManyLogs,
    DuplicateJob,
    Selection(SelectionError),
}

/// Input snapshots retain coordinator order. Filtering affects presentation, never retained IDs.
pub fn build_recent_logs_model(
    jobs: &[UploadJobSnapshot],
    selection: &SessionLogSelection,
    now: SystemTime,
    enabled_providers: ProviderSelection,
) -> Result<RecentLogsModel, RecentLogsModelError> {
    if jobs.len() > MAX_HISTORY_LIMIT {
        return Err(RecentLogsModelError::TooManyLogs);
    }
    let cutoff = selection
        .display_cutoff(now)
        .map_err(RecentLogsModelError::Selection)?;
    let mut retained = HashSet::with_capacity(jobs.len());
    if jobs.iter().any(|job| !retained.insert(job.id)) {
        return Err(RecentLogsModelError::DuplicateJob);
    }
    let rows: Vec<_> = jobs
        .iter()
        .rev()
        .filter(|job| job.record.file.last_write_time >= cutoff)
        .map(build_recent_log_row)
        .collect();
    let urls: Vec<_> = rows
        .iter()
        .filter_map(|row| row.dps_report_url.as_deref())
        .collect();
    let fights: Vec<_> = rows
        .iter()
        .filter_map(|row| row.donbot_fight_log_id)
        .collect();
    Ok(RecentLogsModel {
        retained_job_ids: jobs.iter().map(|job| job.id).collect(),
        enabled_providers,
        dps_report_clipboard_text: urls.join("\n"),
        dps_report_url_count: urls.len(),
        donbot_aggregate_url: aggregate_url(&fights),
        donbot_fight_count: fights.len(),
        rows,
    })
}

pub fn build_recent_log_row(job: &UploadJobSnapshot) -> RecentLogRow {
    let record = &job.record;
    let dps_report_url = report_url(job, ReportTarget::DpsReport).ok();
    let encounter = if let Some(report) = &record.dps_report_result {
        let mut name = report.encounter_name.clone();
        if !report.mode.is_empty() {
            name.push_str(&format!(" ({})", report.mode));
        }
        if !report.success {
            if let Some(health) = record
                .encounter_metadata
                .as_ref()
                .and_then(|metadata| metadata.remaining_health_basis_points)
                .filter(|health| *health <= 10_000)
            {
                let percentage = format!("{}.{:02}", health / 100, health % 100);
                name.push_str(&format!(
                    " ({}% left)",
                    percentage.trim_end_matches('0').trim_end_matches('.')
                ));
            }
        }
        name
    } else if let Some(metadata) = &record.encounter_metadata {
        format!("Encounter {}", metadata.boss_id)
    } else {
        "Reading metadata".into()
    };
    RecentLogRow {
        id: job.id,
        detected_at: format_detected_at(record.detected_at),
        encounter,
        encounter_success: record
            .dps_report_result
            .as_ref()
            .map(|report| report.success),
        rechat_available: dps_report_url.is_some() && !record.providers[3].state.is_busy(),
        dps_report_url,
        donbot_fight_log_id: record
            .donbot_upload_receipt
            .as_ref()
            .and_then(|receipt| receipt.fight_log_id)
            .filter(|id| valid_donbot_fight_id(*id)),
        wingman_report_available: report_url(job, ReportTarget::Wingman).is_ok(),
        directory_available: record
            .file
            .canonical_path
            .parent()
            .is_some_and(|parent| !parent.as_os_str().is_empty()),
        reupload_available: record.encounter_metadata.is_some()
            && record.providers[..3]
                .iter()
                .all(|provider| !provider.state.is_busy()),
        providers: std::array::from_fn(|index| {
            let provider = &record.providers[index];
            ProviderCell {
                state: provider.state,
                status: provider_state_text(provider.state),
                detail: provider.detail.clone(),
                retry_available: provider.state == ProviderState::Failed
                    && record.encounter_metadata.is_some(),
            }
        }),
    }
}
pub fn provider_state_text(state: ProviderState) -> &'static str {
    match state {
        ProviderState::Disabled => "Not selected",
        ProviderState::Waiting => "Waiting",
        ProviderState::Active => "Uploading",
        ProviderState::Succeeded => "Succeeded",
        ProviderState::Skipped => "Skipped",
        ProviderState::RetryScheduled(_) => "Retrying",
        ProviderState::Failed => "Failed",
        ProviderState::Cancelled => "Cancelled",
    }
}

fn aggregate_url(ids: &[u64]) -> String {
    if ids.is_empty() {
        return String::new();
    }
    format!(
        "https://donbot.walmslo.com/logs/aggregate?ids={}",
        ids.iter().map(u64::to_string).collect::<Vec<_>>().join(",")
    )
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DonBotAggregateModel {
    pub selected_jobs: Vec<UploadJobId>,
    pub maximum_fights: usize,
    pub send_available: bool,
    pub selected_aggregate_url: String,
    pub diagnostic: &'static str,
}
/// Selection is adapter-owned. This projection neither changes it nor dispatches delivery.
pub fn build_donbot_aggregate_model(
    recent: &RecentLogsModel,
    selection: &DonBotAggregateSelection,
    options: &NexusOptionsSnapshot,
    delivery: &DonBotAggregateDeliverySnapshot,
) -> DonBotAggregateModel {
    let selected_rows: Vec<_> = recent
        .rows
        .iter()
        .filter(|row| row.donbot_fight_log_id.is_some() && selection.selected(row.id))
        .collect();
    let ids: Vec<_> = selected_rows
        .iter()
        .filter_map(|row| row.donbot_fight_log_id)
        .collect();
    let unique: HashSet<_> = ids.iter().copied().collect();
    let bot = &options.donbot;
    let guild = bot
        .guilds
        .iter()
        .find(|guild| guild.guild_id == bot.selected_guild_id);
    let maximum = guild.map_or(0, |guild| {
        usize::from(guild.discord_delivery.max_aggregate_fight_logs)
            .min(usize::from(MAX_DONBOT_AGGREGATE_FIGHTS))
    });
    let available = options.accepting_commands
        && !options.shutting_down
        && !options.configuration.shutting_down
        && !bot.shutting_down
        && !bot.refreshing
        && !delivery.shutting_down
        && !matches!(
            delivery.state,
            DonBotAggregateDeliveryState::Queued
                | DonBotAggregateDeliveryState::Sending
                | DonBotAggregateDeliveryState::ShuttingDown
        )
        && bot.discord_aggregate_delivery_v1
        && guild.is_some_and(|guild| guild.discord_delivery.aggregate_enabled)
        && authorized_donbot_aggregate_route(&options.configuration.settings, bot).requested()
        && (2..=maximum).contains(&ids.len())
        && unique.len() == ids.len();
    DonBotAggregateModel {
        selected_jobs: selected_rows.iter().map(|row| row.id).collect(),
        maximum_fights: maximum,
        send_available: available,
        selected_aggregate_url: aggregate_url(&ids),
        diagnostic: delivery.detail(),
    }
}

/// UTC formatting uses Gregorian cycles and is independent of platform locales and time zones.
pub fn format_detected_at(time: SystemTime) -> String {
    let seconds = match time.duration_since(SystemTime::UNIX_EPOCH) {
        Ok(duration) => i128::from(duration.as_secs()),
        Err(error) => {
            -i128::from(error.duration().as_secs())
                - i128::from(error.duration().subsec_nanos() != 0)
        }
    };
    format_utc_seconds(seconds)
}

fn format_utc_seconds(seconds: i128) -> String {
    let days = seconds.div_euclid(86_400) + 719_162;
    if !(0..3_652_059).contains(&days) {
        return "Time unavailable".into();
    }
    let mut remaining = days % 146_097;
    let centuries = (remaining / 36_524).min(3);
    remaining -= centuries * 36_524;
    let groups = remaining / 1461;
    remaining %= 1461;
    let years = (remaining / 365).min(3);
    remaining -= years * 365;
    let year = 1 + (days / 146_097) * 400 + centuries * 100 + groups * 4 + years;
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let month_days = [
        31,
        if leap { 29 } else { 28 },
        31,
        30,
        31,
        30,
        31,
        31,
        30,
        31,
        30,
        31,
    ];
    let mut month = 1;
    for length in month_days {
        if remaining < length {
            break;
        }
        remaining -= length;
        month += 1;
    }
    let day_seconds = seconds.rem_euclid(86_400);
    format!(
        "{year:04}-{month:02}-{:02} {:02}:{:02}:{:02} UTC",
        remaining + 1,
        day_seconds / 3600,
        day_seconds / 60 % 60,
        day_seconds % 60
    )
}

#[cfg(test)]
mod tests {
    use super::format_utc_seconds;

    #[test]
    fn gregorian_dates_before_the_windows_filetime_epoch() {
        for (seconds, expected) in [
            (-62_135_596_801, "Time unavailable"),
            (-62_135_596_800, "0001-01-01 00:00:00 UTC"),
            (-11_676_096_000, "1600-01-01 00:00:00 UTC"),
            (-11_670_998_400, "1600-02-29 00:00:00 UTC"),
        ] {
            assert_eq!(format_utc_seconds(seconds), expected);
        }
    }
}
