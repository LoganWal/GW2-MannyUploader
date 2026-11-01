mod options;
mod recent;
#[cfg(test)]
mod tests;
mod widgets;

use manny_core::{application::*, ui::*};
use widgets::{Frame, TextBuffer};

pub(crate) struct Renderer {
    ordinary: NexusOrdinaryOptions,
    log_directory: TextBuffer,
    template: TextBuffer,
    donbot_base: TextBuffer,
    donbot_key: TextBuffer,
    dirty: bool,
    endpoint_dirty: bool,
    donbot_setup: bool,
    submitted: Option<NexusOrdinaryOptions>,
    selection: DonBotAggregateSelection,
    error: Option<&'static str>,
}
impl Renderer {
    pub fn new(snapshot: &RuntimeSnapshot) -> Self {
        let ordinary = NexusOrdinaryOptions::from(&snapshot.options.configuration.settings);
        let mut result = Self {
            ordinary,
            log_directory: TextBuffer::new(4096),
            template: TextBuffer::new(2000),
            donbot_base: TextBuffer::new(2048),
            donbot_key: TextBuffer::new(MAX_DONBOT_API_KEY_BYTES),
            dirty: false,
            endpoint_dirty: false,
            donbot_setup: false,
            submitted: None,
            selection: DonBotAggregateSelection::default(),
            error: None,
        };
        result.sync(snapshot);
        result
    }
    fn sync(&mut self, snapshot: &RuntimeSnapshot) {
        let settings = &snapshot.options.configuration.settings;
        if let Some(submitted) = &self.submitted {
            let mut applied = NexusOrdinaryOptions::from(settings);
            applied.general.window_visible = submitted.general.window_visible;
            if &applied == submitted {
                let mut draft = self.draft(snapshot);
                draft.general.window_visible = submitted.general.window_visible;
                if &draft == submitted {
                    self.dirty = false;
                }
                self.submitted = None;
            }
        }
        if !self.dirty {
            self.ordinary = NexusOrdinaryOptions::from(settings);
            self.log_directory.set(&settings.general.log_directory);
            self.template.set(&settings.twitch.message_template);
        }
        if !self.endpoint_dirty {
            self.donbot_base.set(&settings.donbot.api_base_url);
        }
        if matches!(
            snapshot.options.donbot.state,
            DonBotConfigurationState::Verified
                | DonBotConfigurationState::Verifying
                | DonBotConfigurationState::ShuttingDown
        ) {
            self.donbot_key.clear();
            self.endpoint_dirty = false;
        }
    }
    fn draft(&self, snapshot: &RuntimeSnapshot) -> NexusOrdinaryOptions {
        let mut draft = self.ordinary.clone();
        draft.general.log_directory = self.log_directory.text().into();
        draft.twitch_message_template = self.template.text().into();
        draft.general.window_visible = snapshot
            .options
            .configuration
            .settings
            .general
            .window_visible;
        draft
    }
    fn command(&mut self, handle: &RuntimeHandle, command: NexusOptionsCommand) -> bool {
        match handle.submit_options(command) {
            Ok(()) => {
                self.error = None;
                true
            }
            Err(error) => {
                self.error = Some(error.detail());
                false
            }
        }
    }
    /// # Safety
    /// Must run in an admitted Nexus options callback inside the host's active ImGui frame.
    pub unsafe fn options(&mut self, snapshot: &RuntimeSnapshot, handle: &RuntimeHandle) {
        // SAFETY: Forward the caller's active-frame and callback-admission contract.
        let ui = unsafe { Frame::new() };
        self.sync(snapshot);
        self.render_options(&ui, snapshot, handle);
    }
}
