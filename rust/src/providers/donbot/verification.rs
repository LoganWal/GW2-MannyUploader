use super::*;
use crate::domain::canonical_discord_id;
use crate::providers::json::{self, List, Object, Text};
use serde::de::{IgnoredAny, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use std::collections::HashSet;
use std::fmt;

#[derive(Deserialize)]
struct Counts {
    guilds: List<Object<CountGuild>, MAX_DONBOT_GUILDS>,
}
#[derive(Deserialize)]
struct CountGuild {
    #[serde(rename = "discordDelivery")]
    policy: Option<Object<CountPolicy>>,
}
#[derive(Deserialize)]
struct CountPolicy {
    channels: Option<List<IgnoredAny, MAX_DONBOT_CHANNELS_PER_GUILD>>,
}

pub(super) fn decode(response: &HttpResponse) -> Result<DonBotVerification, DonBotError> {
    let invalid = || DonBotError {
        http_status: Some(response.status_code),
        ..DonBotError::failed(DonBotErrorCode::InvalidResponse)
    };
    let bytes = response.body.expose();
    // Count ignored channel records before allocating their names and IDs across the response.
    let counts: Counts =
        json::decode(bytes, MAX_DONBOT_VERIFICATION_BYTES).map_err(|_| invalid())?;
    let count: usize = counts
        .guilds
        .0
        .iter()
        .map(|guild| {
            guild
                .0
                .policy
                .as_ref()
                .and_then(|policy| policy.0.channels.as_ref())
                .map_or(0, |channels| channels.0.len())
        })
        .sum();
    if count > MAX_DONBOT_CHANNELS {
        return Err(invalid());
    }
    let raw: Verification =
        json::decode(bytes, MAX_DONBOT_VERIFICATION_BYTES).map_err(|_| invalid())?;
    let capability = raw.capabilities.unwrap_or_default();
    if !display_name(&raw.account_name.0) || (capability.aggregate && !capability.summary) {
        return Err(invalid());
    }
    let mut guilds = Vec::with_capacity(raw.guilds.0.len());
    let mut ids = HashSet::new();
    for Object(guild) in raw.guilds.0 {
        if !canonical_discord_id(&guild.guild_id.0)
            || !display_name(&guild.guild_name.0)
            || !ids.insert(guild.guild_id.0.clone())
        {
            return Err(invalid());
        }
        let policy = if capability.summary {
            policy(
                guild.discord_delivery.ok_or_else(invalid)?.0,
                capability.aggregate,
            )
            .ok_or_else(invalid)?
        } else {
            DonBotDiscordDeliveryPolicy::default()
        };
        guilds.push(DonBotGuild {
            guild_id: guild.guild_id.0,
            guild_name: guild.guild_name.0,
            discord_delivery: policy,
        });
    }
    Ok(DonBotVerification {
        account_name: raw.account_name.0,
        discord_summary_delivery_v1: capability.summary,
        discord_aggregate_delivery_v1: capability.aggregate,
        guilds,
    })
}
fn display_name(value: &str) -> bool {
    !value.is_empty() && !value.bytes().any(|b| b < 0x20 || b == 0x7f)
}
fn policy(raw: Policy, aggregate: bool) -> Option<DonBotDiscordDeliveryPolicy> {
    let mut policy = DonBotDiscordDeliveryPolicy {
        enabled: raw.enabled?,
        defaults_available: raw.defaults_available?,
        channel_override_allowed: raw.channel_override_allowed?,
        ..DonBotDiscordDeliveryPolicy::default()
    };
    if aggregate {
        let maximum = raw.max_aggregate_fight_logs?;
        if !(2..=MAX_DONBOT_AGGREGATE_FIGHTS).contains(&maximum) {
            return None;
        }
        policy.aggregate_enabled = raw.aggregate_enabled?;
        policy.aggregate_defaults_available = raw.aggregate_defaults_available?;
        policy.max_aggregate_fight_logs = maximum;
    }
    for Text(kind) in raw.enabled_message_kinds?.0 {
        let flag = match kind.as_str() {
            "pve-summary" => &mut policy.pve_summary,
            "wvw-summary" => &mut policy.wvw_summary,
            "wvw-advanced" => &mut policy.wvw_advanced,
            "wvw-stream" => &mut policy.wvw_stream,
            _ => return None,
        };
        if *flag {
            return None;
        }
        *flag = true;
    }
    let mut ids = HashSet::new();
    for Object(channel) in raw.channels?.0 {
        if !canonical_discord_id(&channel.channel_id.0)
            || !display_name(&channel.channel_name.0)
            || !ids.insert(channel.channel_id.0.clone())
        {
            return None;
        }
        policy.channels.push(DonBotChannel {
            channel_id: channel.channel_id.0,
            channel_name: channel.channel_name.0,
        });
    }
    Some(policy)
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Verification {
    account_name: Text<256>,
    guilds: List<Object<Guild>, MAX_DONBOT_GUILDS>,
    capabilities: Option<Capabilities>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Guild {
    guild_id: Text<19>,
    guild_name: Text<256>,
    discord_delivery: Option<Object<Policy>>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Policy {
    enabled: Option<bool>,
    defaults_available: Option<bool>,
    channel_override_allowed: Option<bool>,
    enabled_message_kinds: Option<List<Text<32>, 4>>,
    channels: Option<List<Object<Channel>, MAX_DONBOT_CHANNELS_PER_GUILD>>,
    aggregate_enabled: Option<bool>,
    aggregate_defaults_available: Option<bool>,
    max_aggregate_fight_logs: Option<u16>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Channel {
    channel_id: Text<19>,
    channel_name: Text<256>,
}

#[derive(Default)]
struct Capabilities {
    summary: bool,
    aggregate: bool,
}
enum Capability {
    Summary,
    Aggregate,
    Unknown,
}
impl<'de> Deserialize<'de> for Capability {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct Read;
        impl Visitor<'_> for Read {
            type Value = Capability;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("capability text")
            }
            fn visit_str<E: serde::de::Error>(self, value: &str) -> Result<Self::Value, E> {
                Ok(match value {
                    "discord-summary-delivery-v1" => Capability::Summary,
                    "discord-aggregate-delivery-v1" => Capability::Aggregate,
                    _ => Capability::Unknown,
                })
            }
        }
        deserializer.deserialize_str(Read)
    }
}
impl<'de> Deserialize<'de> for Capabilities {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct Read;
        impl<'de> Visitor<'de> for Read {
            type Value = Capabilities;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("capabilities")
            }
            fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Self::Value, A::Error> {
                let mut result = Capabilities::default();
                while let Some(value) = seq.next_element::<Capability>()? {
                    match value {
                        Capability::Summary => result.summary = true,
                        Capability::Aggregate => result.aggregate = true,
                        Capability::Unknown => {}
                    }
                }
                Ok(result)
            }
        }
        deserializer.deserialize_seq(Read)
    }
}
