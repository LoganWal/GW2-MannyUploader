use super::*;
use manny_core::domain::Provider;
use std::time::SystemTime;

impl Renderer {
    /// # Safety
    /// Must run in an admitted Nexus render callback inside the host's active ImGui frame.
    pub unsafe fn main(
        &mut self,
        snapshot: &RuntimeSnapshot,
        handle: &RuntimeHandle,
        visible: &mut bool,
    ) {
        // SAFETY: Forward the caller's active-frame and callback-admission contract.
        let ui = unsafe { Frame::new() };
        self.sync(snapshot);
        if !*visible {
            return;
        }
        ui.window(visible, || {
            self.destinations(&ui, snapshot, handle);
            self.donbot(&ui, snapshot, handle, false);
            self.twitch(&ui, snapshot, handle, false);
            if let Some(error) = snapshot.options.last_error.as_ref() {
                ui.warning(error.detail());
            }
            ui.separator();
            for (title, mode) in [
                ("Show New", LogSelectionMode::New),
                ("Show Last 24 Hours", LogSelectionMode::Last24Hours),
            ] {
                let selected = snapshot.selection.mode() == mode;
                if ui.button(title, !selected && !snapshot.shutting_down) {
                    if let Err(error) = handle.select_logs(mode) {
                        self.error = Some(error.detail());
                    }
                }
                ui.same_line();
            }
            ui.text(if snapshot.root_available {
                "Watching logs"
            } else {
                "Waiting for the log folder"
            });
            if let Some(error) = &snapshot.diagnostic {
                ui.warning(error.detail());
            }
            if !snapshot.source_issues.is_empty() {
                ui.warning("Some log entries could not be inspected");
            }
            if snapshot.history_recovery.is_some() {
                ui.warning("Upload history could not be fully recovered");
            }
            if let Some(error) = self.error {
                ui.warning(error);
            }
            if let Some(error) = snapshot.recent_actions.last_error {
                ui.warning(error.detail());
            }
            self.logs(&ui, snapshot, handle);
        });
    }

    fn action(&mut self, handle: &RuntimeHandle, action: RecentLogAction) {
        self.error = handle
            .submit_action(action)
            .err()
            .map(RecentLogActionError::detail);
    }

    fn logs(&mut self, ui: &Frame, snapshot: &RuntimeSnapshot, handle: &RuntimeHandle) {
        let options = &snapshot.options;
        let settings = &options.configuration.settings;
        let recent = match build_recent_logs_model(
            &snapshot.jobs,
            &snapshot.selection,
            SystemTime::now(),
            settings.enabled_providers(),
        ) {
            Ok(recent) => recent,
            Err(_) => {
                ui.warning("Recent logs could not be displayed");
                return;
            }
        };
        let bot = &options.donbot;
        let identity = bot
            .account_name
            .as_ref()
            .map(|account| DonBotSelectionIdentity {
                api_base_url: bot.api_base_url.clone(),
                account_name: account.clone(),
                guild_id: bot.selected_guild_id.clone(),
            });
        let candidates: Vec<_> = recent
            .rows
            .iter()
            .map(RecentLogRow::aggregate_candidate)
            .collect();
        self.selection
            .reconcile(identity, &recent.retained_job_ids, &candidates);
        if ui.button("Copy dps.report URLs", recent.dps_report_url_count != 0) {
            ui.copy(&recent.dps_report_clipboard_text);
        }
        ui.same_line();
        let aggregate =
            build_donbot_aggregate_model(&recent, &self.selection, options, &snapshot.aggregate);
        if ui.button(
            "Copy DonBot aggregate URL",
            !aggregate.selected_aggregate_url.is_empty(),
        ) {
            ui.copy(&aggregate.selected_aggregate_url);
        }
        if ui.button(
            "Send selected logs via DonBot aggregate",
            aggregate.send_available,
        ) {
            self.error = handle
                .submit_aggregate(SendDonBotAggregateCommand {
                    job_ids: aggregate.selected_jobs,
                    configuration_revision: options.configuration.revision,
                    donbot_revision: bot.revision,
                })
                .err()
                .map(DonBotAggregateDeliveryError::detail);
        }
        if !aggregate.diagnostic.is_empty() {
            ui.text(aggregate.diagnostic);
        }
        let providers: Vec<_> = [
            Provider::DpsReport,
            Provider::Wingman,
            Provider::DonBot,
            Provider::Twitch,
        ]
        .into_iter()
        .filter(|provider| recent.enabled_providers.contains(*provider))
        .collect();
        let mut columns = vec!["Select", "Detected", "Encounter"];
        columns.extend(providers.iter().map(|provider| provider.name()));
        columns.push("Actions");
        ui.table(&columns, || {
            for row in &recent.rows {
                ui.id(&row.id.get().to_string(), || {
                    ui.next_row();
                    ui.column();
                    let eligible = row.donbot_fight_log_id.is_some();
                    let mut selected = eligible && self.selection.selected(row.id);
                    if ui.checkbox("##aggregate", &mut selected, eligible) {
                        self.selection.set_selected(row.id, selected);
                    }
                    ui.column();
                    ui.text(&row.detected_at);
                    ui.column();
                    match row.encounter_success {
                        Some(true) => ui.colored(&row.encounter, [0.3, 0.85, 0.4, 1.0]),
                        Some(false) => ui.colored(&row.encounter, [1.0, 0.4, 0.3, 1.0]),
                        None => ui.text(&row.encounter),
                    }
                    for provider in &providers {
                        ui.column();
                        ui.id(provider.name(), || {
                            let cell = &row.providers[*provider as usize];
                            ui.text(cell.status);
                            if !cell.detail.is_empty() {
                                ui.text(&cell.detail);
                            }
                            let target = match provider {
                                Provider::DpsReport if row.dps_report_url.is_some() => {
                                    Some(ReportTarget::DpsReport)
                                }
                                Provider::Wingman if row.wingman_report_available => {
                                    Some(ReportTarget::Wingman)
                                }
                                Provider::DonBot if row.donbot_fight_log_id.is_some() => {
                                    Some(ReportTarget::DonBot)
                                }
                                _ => None,
                            };
                            if let Some(target) = target {
                                if ui.button("Open report", true) {
                                    self.action(
                                        handle,
                                        RecentLogAction::OpenReport {
                                            job_id: row.id,
                                            target,
                                        },
                                    );
                                }
                            }
                            if cell.retry_available && ui.button("Retry", !snapshot.shutting_down) {
                                self.action(
                                    handle,
                                    RecentLogAction::Retry {
                                        job_id: row.id,
                                        provider: *provider,
                                    },
                                );
                            }
                        });
                    }
                    ui.column();
                    self.row_actions(ui, handle, row, snapshot.shutting_down);
                });
            }
        });
        if recent.rows.is_empty() {
            ui.text("No logs in this view");
        }
    }

    fn row_actions(
        &mut self,
        ui: &Frame,
        handle: &RuntimeHandle,
        row: &RecentLogRow,
        stopping: bool,
    ) {
        if ui.button("Open folder", row.directory_available && !stopping) {
            self.action(handle, RecentLogAction::OpenDirectory(row.id));
        }
        if ui.button("Reupload", row.reupload_available && !stopping) {
            self.action(handle, RecentLogAction::Reupload(row.id));
        }
        if ui.button("Rechat", row.rechat_available && !stopping) {
            self.action(handle, RecentLogAction::Rechat(row.id));
        }
    }
}
