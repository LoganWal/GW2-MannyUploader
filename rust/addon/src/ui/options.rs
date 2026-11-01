use super::*;

impl Renderer {
    pub(super) fn render_options(
        &mut self,
        ui: &Frame,
        snapshot: &RuntimeSnapshot,
        handle: &RuntimeHandle,
    ) {
        let model = build_nexus_options_model(&snapshot.options);
        ui.text("MannyUploader");
        ui.text(model.protected_storage_text);
        if let Some(error) = self.error.or(model.last_error) {
            ui.warning(error);
            if ui.button("Dismiss error", true) {
                self.command(handle, NexusOptionsCommand::DismissError);
            }
        }
        let mut visible = snapshot
            .options
            .configuration
            .settings
            .general
            .window_visible;
        if ui.checkbox(
            "Show MannyUploader list",
            &mut visible,
            model.settings_editable,
        ) {
            self.command(handle, NexusOptionsCommand::SetWindowVisible(visible));
        }
        self.destinations(ui, snapshot, handle);
        ui.separator();
        ui.text("Log discovery");
        self.dirty |= ui.input(
            "Log folder",
            &mut self.log_directory,
            false,
            model.settings_editable,
        );
        self.dirty |= ui.checkbox(
            "Include subdirectories",
            &mut self.ordinary.general.watch_subdirectories,
            model.settings_editable,
        );
        for (title, value, range) in [
            (
                "Poll interval (ms)",
                &mut self.ordinary.general.poll_interval_ms,
                250..=60_000,
            ),
            (
                "Matching observations",
                &mut self.ordinary.general.stability_observations,
                2..=10,
            ),
            (
                "Recent log limit",
                &mut self.ordinary.general.recent_log_limit,
                1..=500,
            ),
            (
                "Parser queue capacity",
                &mut self.ordinary.general.parser_queue_capacity,
                1..=64,
            ),
            (
                "Parallel uploads per destination",
                &mut self.ordinary.general.parallel_uploads_per_provider,
                1..=32,
            ),
            (
                "Maximum log candidates",
                &mut self.ordinary.general.max_candidates,
                1..=10_000,
            ),
        ] {
            self.dirty |= ui.integer(title, value, range, model.settings_editable);
        }
        ui.separator();
        self.donbot(ui, snapshot, handle, true);
        ui.separator();
        self.twitch(ui, snapshot, handle, true);
        ui.separator();
        if ui.button("Save settings", model.save_available && self.dirty) {
            // Visibility has its own command and may have changed while this draft was edited.
            let draft = self.draft(snapshot);
            if self.command(handle, NexusOptionsCommand::SaveOrdinary(draft.clone())) {
                self.submitted = Some(draft);
            }
        }
        ui.same_line();
        if ui.button("Discard changes", self.dirty) {
            self.dirty = false;
            self.sync(snapshot);
        }
        if self.dirty {
            ui.text("Unsaved settings");
        }
        if model.command_pending {
            ui.text("Applying changes...");
        }
    }

    pub(super) fn destinations(
        &mut self,
        ui: &Frame,
        snapshot: &RuntimeSnapshot,
        handle: &RuntimeHandle,
    ) {
        let options = &snapshot.options;
        let settings = &options.configuration.settings;
        let enabled = options.accepting_commands && !snapshot.shutting_down;
        let mut dps = settings.dps_report.enabled;
        if ui.checkbox("dps.report", &mut dps, enabled) {
            self.command(handle, NexusOptionsCommand::SetDpsReportEnabled(dps));
        }
        ui.same_line();
        let mut wingman = settings.wingman.enabled;
        if ui.checkbox("GW2Wingman", &mut wingman, enabled) {
            self.command(handle, NexusOptionsCommand::SetWingmanEnabled(wingman));
        }
        ui.same_line();
        let bot = build_nexus_options_model(options).donbot;
        let mut donbot = settings.donbot.enabled || self.donbot_setup;
        if ui.checkbox("DonBot", &mut donbot, enabled) {
            self.donbot_setup = donbot;
            if !donbot || bot.enable_toggle_available {
                self.command(handle, NexusOptionsCommand::SetDonBotEnabled(donbot));
            }
        }
        ui.same_line();
        let twitch = build_nexus_options_model(options).twitch;
        if options.twitch.state == TwitchConnectionState::Connected {
            let mut posting = settings.twitch.enabled;
            if ui.checkbox("Twitch", &mut posting, twitch.enable_toggle_available) {
                self.command(handle, NexusOptionsCommand::SetTwitchEnabled(posting));
            }
        } else if twitch.connect_available {
            if ui.button("Connect Twitch", true) {
                self.command(handle, NexusOptionsCommand::ConnectTwitch);
            }
            ui.same_line();
            ui.warning("Authorization opens in your browser. Keep that window off stream.");
        } else {
            ui.text(&twitch.status_text);
        }
        if settings.dps_report.enabled {
            let mut detailed = settings.dps_report.detailed_wvw;
            if ui.checkbox("Detailed WvW", &mut detailed, enabled) {
                self.command(
                    handle,
                    NexusOptionsCommand::SetDpsReportDetailedWvw(detailed),
                );
            }
        }
    }

    pub(super) fn donbot(
        &mut self,
        ui: &Frame,
        snapshot: &RuntimeSnapshot,
        handle: &RuntimeHandle,
        options_page: bool,
    ) {
        let options = &snapshot.options;
        let model = build_nexus_options_model(options).donbot;
        let bot = &options.donbot;
        let settings = &options.configuration.settings.donbot;
        if !settings.enabled && !self.donbot_setup {
            return;
        }
        if self.donbot_setup
            && !settings.enabled
            && model.enable_toggle_available
            && options.pending_commands == 0
        {
            self.command(handle, NexusOptionsCommand::SetDonBotEnabled(true));
        }
        if settings.enabled {
            self.donbot_setup = false;
        }
        let verified =
            bot.state == DonBotConfigurationState::Verified && bot.account_name.is_some();
        ui.text("DonBot");
        ui.text(&model.status_text);
        if let Some(error) = model.diagnostic {
            ui.warning(error);
        }
        if model.verification_in_progress {
            return;
        }
        if !verified {
            self.endpoint_dirty |= ui.input(
                "DonBot API endpoint",
                &mut self.donbot_base,
                false,
                model.verify_available,
            );
            ui.input(
                "GW2 API key",
                &mut self.donbot_key,
                true,
                model.verify_available,
            );
            if ui.button("Verify DonBot", model.verify_available) {
                let api_key = self.donbot_key.take_secret();
                self.command(
                    handle,
                    NexusOptionsCommand::VerifyDonBot {
                        api_base_url: self.donbot_base.text().into(),
                        api_key,
                    },
                );
            }
        }
        if verified {
            let guild = bot
                .guilds
                .iter()
                .find(|guild| guild.guild_id == bot.selected_guild_id);
            let preview = guild.map_or("Select server", |guild| guild.guild_name.as_str());
            ui.combo("Server", preview, model.guild_selection_available, || {
                for guild in &bot.guilds {
                    ui.id(&guild.guild_id, || {
                        if ui.selectable(
                            &escape_imgui_label_text(&guild.guild_name),
                            guild.guild_id == bot.selected_guild_id,
                        ) {
                            self.command(
                                handle,
                                NexusOptionsCommand::SelectDonBotGuild(guild.guild_id.clone()),
                            );
                        }
                    });
                }
            });
            ui.same_line();
        }
        if model.discord_channel_selection_visible {
            if let Some(guild) = bot
                .guilds
                .iter()
                .find(|guild| guild.guild_id == bot.selected_guild_id)
            {
                let preview = guild
                    .discord_delivery
                    .channels
                    .iter()
                    .find(|channel| channel.channel_id == settings.selected_discord_channel_id)
                    .map_or("Server defaults", |channel| channel.channel_name.as_str());
                ui.combo(
                    "Discord channel",
                    preview,
                    model.discord_channel_selection_available,
                    || {
                        if ui.selectable(
                            "Server defaults",
                            !settings.discord_channel_override_explicit,
                        ) {
                            self.command(
                                handle,
                                NexusOptionsCommand::SelectDonBotDiscordChannel(String::new()),
                            );
                        }
                        for channel in &guild.discord_delivery.channels {
                            ui.id(&channel.channel_id, || {
                                if ui.selectable(
                                    &escape_imgui_label_text(&channel.channel_name),
                                    channel.channel_id == settings.selected_discord_channel_id,
                                ) {
                                    self.command(
                                        handle,
                                        NexusOptionsCommand::SelectDonBotDiscordChannel(
                                            channel.channel_id.clone(),
                                        ),
                                    );
                                }
                            });
                        }
                    },
                );
                ui.same_line();
            }
        }
        if model.discord_delivery_visible {
            let mut delivery = settings.discord_delivery_enabled;
            if ui.checkbox(
                "Auto post to Discord",
                &mut delivery,
                model.discord_delivery_toggle_available,
            ) {
                self.command(
                    handle,
                    NexusOptionsCommand::SetDonBotDiscordDeliveryEnabled(delivery),
                );
            }
        } else if settings.enabled {
            ui.text(model.discord_delivery_status_text);
        }
        if options_page && model.disconnect_available {
            if verified && ui.button("Refresh DonBot servers", model.refresh_available) {
                self.command(handle, NexusOptionsCommand::RefreshDonBot);
            }
            ui.same_line();
            if ui.button("Disconnect DonBot", model.disconnect_available) {
                self.command(handle, NexusOptionsCommand::DisconnectDonBot);
            }
        }
    }

    pub(super) fn twitch(
        &mut self,
        ui: &Frame,
        snapshot: &RuntimeSnapshot,
        handle: &RuntimeHandle,
        options_page: bool,
    ) {
        let model = build_nexus_options_model(&snapshot.options).twitch;
        let connected = snapshot.options.twitch.state == TwitchConnectionState::Connected;
        if snapshot.options.twitch.state == TwitchConnectionState::Disconnected {
            return;
        }
        ui.text("Twitch");
        ui.text(&model.status_text);
        if let Some(error) = model.diagnostic {
            ui.warning(error);
        }
        if model.authorization_available {
            if let Some(mask) = model.masked_user_code {
                ui.text(mask);
            }
            if ui.button("Open Twitch authorization", true) {
                self.command(
                    handle,
                    NexusOptionsCommand::OpenTwitchAuthorization {
                        revision: snapshot.options.twitch.revision,
                    },
                );
            }
            ui.warning("The authorization code will be visible in your browser. Keep that window off stream.");
        }
        if !connected {
            if model.disconnect_available && ui.button("Disconnect Twitch", true) {
                self.command(handle, NexusOptionsCommand::DisconnectTwitch);
            }
            return;
        }
        if options_page && snapshot.options.configuration.settings.twitch.enabled {
            let editable = snapshot.options.accepting_commands && !snapshot.shutting_down;
            self.dirty |= ui.checkbox(
                "Post successful encounters",
                &mut self.ordinary.twitch_post_success,
                editable,
            );
            self.dirty |= ui.checkbox(
                "Post failed encounters",
                &mut self.ordinary.twitch_post_failure,
                editable,
            );
            self.dirty |= ui.input("Chat message template", &mut self.template, false, editable);
            if ui.button("Send test message", model.test_message_available) {
                self.command(handle, NexusOptionsCommand::SendTwitchTestMessage);
            }
            ui.same_line();
            if ui.button("Disconnect Twitch", model.disconnect_available) {
                self.command(handle, NexusOptionsCommand::DisconnectTwitch);
            }
            ui.text(model.test_message_status_text);
            if let Some(error) = model.test_message_diagnostic {
                ui.warning(error);
            }
        } else if options_page && model.disconnect_available && ui.button("Disconnect Twitch", true)
        {
            self.command(handle, NexusOptionsCommand::DisconnectTwitch);
        }
    }
}
