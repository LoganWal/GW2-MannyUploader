#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EncounterMetadata {
    pub boss_id: u16,
    pub pov_account: String,
    pub remaining_health_basis_points: Option<u16>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DpsReportResult {
    pub permalink: String,
    pub encounter_name: String,
    pub boss_id: u16,
    pub mode: String,
    pub success: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WingmanUploadReceipt {
    pub permalink: String,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DonBotDiscordDeliveryOutcome {
    #[default]
    NotRequested,
    Sent,
    Partial,
    Skipped,
    Failed,
    Ambiguous,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DonBotDiscordDeliveryReceipt {
    pub outcome: DonBotDiscordDeliveryOutcome,
    pub sent: u16,
    pub skipped: u16,
    pub failed: u16,
    pub ambiguous: u16,
}

impl DonBotDiscordDeliveryReceipt {
    fn is_valid(&self) -> bool {
        let counts = [self.sent, self.skipped, self.failed, self.ambiguous];
        let total: u32 = counts.iter().map(|&count| u32::from(count)).sum();
        if total > 4 {
            return false;
        }
        let populated = counts.iter().filter(|&&count| count != 0).count();
        match self.outcome {
            DonBotDiscordDeliveryOutcome::NotRequested => total == 0,
            DonBotDiscordDeliveryOutcome::Sent => self.sent > 0 && populated == 1,
            DonBotDiscordDeliveryOutcome::Partial => populated >= 2,
            DonBotDiscordDeliveryOutcome::Skipped => self.skipped > 0 && populated == 1,
            DonBotDiscordDeliveryOutcome::Failed => self.failed > 0 && populated == 1,
            DonBotDiscordDeliveryOutcome::Ambiguous => self.ambiguous > 0 && populated == 1,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DonBotUploadReceipt {
    pub upload_id: Option<u64>,
    pub fight_log_id: Option<u64>,
    pub discord_delivery: DonBotDiscordDeliveryReceipt,
    pub guild_id: Option<String>,
}

impl DonBotUploadReceipt {
    pub(super) fn is_valid(&self) -> bool {
        self.upload_id.is_none_or(|id| id != 0)
            && self
                .fight_log_id
                .is_none_or(|id| id > 0 && id <= i64::MAX as u64)
            && self.discord_delivery.is_valid()
            && self
                .guild_id
                .as_ref()
                .is_none_or(|id| canonical_discord_id(id))
    }
}

pub(crate) fn canonical_discord_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 19
        && !value.starts_with('0')
        && value.bytes().all(|b| b.is_ascii_digit())
        && value.parse::<i64>().is_ok_and(|id| id > 0)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TwitchDeliveryStatus {
    Sent,
    AutoMod,
    BlockedTerm,
    Duplicate,
    RateLimited,
    FollowersOnly,
    SlowMode,
    SubscribersOnly,
    Restricted,
    OtherDrop,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TwitchDeliveryReceipt {
    pub status: TwitchDeliveryStatus,
    pub message_id: Option<String>,
}

impl TwitchDeliveryReceipt {
    pub(super) fn is_valid(&self) -> bool {
        match (&self.status, &self.message_id) {
            (TwitchDeliveryStatus::Sent, Some(id)) => {
                !id.is_empty()
                    && id.len() <= 256
                    && id.bytes().all(|byte| (0x21..=0x7e).contains(&byte))
            }
            (TwitchDeliveryStatus::Sent, None) => false,
            (_, id) => id.is_none(),
        }
    }
}
