use super::DonBotDeliveryRoute;
use crate::domain::Provider;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ProviderPolicy {
    DpsReport {
        detailed_wvw: bool,
    },
    Wingman,
    DonBot {
        api_base_url: String,
        guild_id: String,
        delivery: DonBotDeliveryRoute,
    },
    Twitch {
        message_template: String,
        post_success: bool,
        post_failure: bool,
    },
}
impl ProviderPolicy {
    pub fn provider(&self) -> Provider {
        match self {
            Self::DpsReport { .. } => Provider::DpsReport,
            Self::Wingman => Provider::Wingman,
            Self::DonBot { .. } => Provider::DonBot,
            Self::Twitch { .. } => Provider::Twitch,
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProviderConfiguration {
    pub parallelism: usize,
    pub policy: ProviderPolicy,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProviderConfigurationError {
    InvalidConfiguration,
    Unavailable,
    ResourceLimit,
    Stopping,
    Panicked,
}
