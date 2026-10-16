use crate::{
    application::DonBotConfigurationSnapshot,
    domain::{Provider, Settings},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum QuickAccessTint {
    Idle,
    Upload,
    Twitch,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QuickAccessStatus {
    pub tooltip: String,
    pub tint: QuickAccessTint,
}

pub fn build_quick_access_status(
    settings: &Settings,
    donbot: &DonBotConfigurationSnapshot,
) -> QuickAccessStatus {
    let selected = settings.enabled_providers();
    let mut lines = vec!["View MannyUploader list".into()];
    let mut tint = QuickAccessTint::Idle;
    for provider in [Provider::DpsReport, Provider::Wingman, Provider::DonBot] {
        if !selected.contains(provider) {
            continue;
        }
        tint = QuickAccessTint::Upload;
        let mut line = format!("Uploading to {}", provider.name());
        if provider == Provider::DonBot {
            let guild = donbot
                .guilds
                .iter()
                .find(|guild| guild.guild_id == settings.donbot.selected_guild_id);
            let name = guild.map_or(settings.donbot.selected_guild_id.as_str(), |guild| {
                guild.guild_name.as_str()
            });
            if !name.is_empty() {
                line.push_str(&format!(" - {}", single_line(name)));
            }
            lines.push(line);
            if settings.donbot.discord_delivery_enabled {
                let route = if settings.donbot.discord_channel_override_explicit {
                    guild
                        .and_then(|guild| {
                            guild.discord_delivery.channels.iter().find(|channel| {
                                channel.channel_id == settings.donbot.selected_discord_channel_id
                            })
                        })
                        .map_or_else(
                            || "selected channel".into(),
                            |channel| format!("#{}", single_line(&channel.channel_name)),
                        )
                } else {
                    "guild defaults".into()
                };
                lines.push(format!("Posting DonBot summaries - {route}"));
            }
        } else {
            lines.push(line);
        }
    }
    if selected.twitch {
        tint = QuickAccessTint::Twitch;
        lines.push("Reporting to Twitch chat".into());
    }
    QuickAccessStatus {
        tooltip: lines.join("\n"),
        tint,
    }
}
fn single_line(text: &str) -> String {
    text.chars()
        .map(|character| {
            if character.is_control() {
                ' '
            } else {
                character
            }
        })
        .collect()
}
