use super::*;
use nexus::imgui::sys as imgui;
use std::{
    ffi::c_void,
    path::PathBuf,
    ptr,
    sync::atomic::{AtomicU64, Ordering},
};

static NEXT: AtomicU64 = AtomicU64::new(1);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "manny-ui-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn runtime(&self) -> ApplicationRuntime<crate::composition::NativeClock> {
        crate::composition::compose(
            &self.0.join("game"),
            &self.0.join("addon"),
            crate::composition::PACKAGED_TWITCH_CLIENT_ID.into(),
        )
        .unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}

unsafe extern "C" {
    fn malloc(size: usize) -> *mut c_void;
    fn free(memory: *mut c_void);
}
unsafe extern "C" fn allocate(size: usize, _: *mut c_void) -> *mut c_void {
    // SAFETY: Forward the allocation request to this test executable's CRT.
    unsafe { malloc(size) }
}
unsafe extern "C" fn deallocate(memory: *mut c_void, _: *mut c_void) {
    // SAFETY: ImGui returns only allocations obtained through allocate.
    unsafe {
        free(memory);
    }
}
struct Gui {
    context: *mut imgui::ImGuiContext,
    _guard: std::sync::MutexGuard<'static, ()>,
}
impl Gui {
    fn new() -> Self {
        let guard = crate::IMGUI_TEST_LOCK.lock().unwrap();
        // SAFETY: This fixture serializes access to the process-global ImGui context and owns it.
        unsafe {
            imgui::igSetAllocatorFunctions(Some(allocate), Some(deallocate), ptr::null_mut());
            let context = imgui::igCreateContext(ptr::null_mut());
            let io = &mut *imgui::igGetIO();
            io.IniFilename = ptr::null();
            io.DisplaySize = imgui::ImVec2 {
                x: 1600.0,
                y: 1200.0,
            };
            io.DeltaTime = 1.0 / 60.0;
            let (mut pixels, mut width, mut height, mut channels) = (ptr::null_mut(), 0, 0, 0);
            imgui::ImFontAtlas_GetTexDataAsRGBA32(
                io.Fonts,
                &mut pixels,
                &mut width,
                &mut height,
                &mut channels,
            );
            Self {
                context,
                _guard: guard,
            }
        }
    }
    fn options(
        &self,
        renderer: &mut Renderer,
        snapshot: &RuntimeSnapshot,
        handle: &RuntimeHandle,
        mouse: [f32; 2],
        down: bool,
    ) -> Vec<(String, [f32; 4])> {
        widgets::TRACE.lock().unwrap().clear();
        // SAFETY: This fixture owns the active context and opens a real frame/window around the renderer.
        unsafe {
            let io = &mut *imgui::igGetIO();
            io.MousePos = imgui::ImVec2 {
                x: mouse[0],
                y: mouse[1],
            };
            io.MouseDown[0] = down;
            imgui::igNewFrame();
            imgui::igSetNextWindowPos(
                imgui::ImVec2 { x: 10.0, y: 10.0 },
                imgui::ImGuiCond_Always as i32,
                imgui::ImVec2 { x: 0.0, y: 0.0 },
            );
            imgui::igSetNextWindowSize(
                imgui::ImVec2 {
                    x: 700.0,
                    y: 1100.0,
                },
                imgui::ImGuiCond_Always as i32,
            );
            imgui::igBegin(c"Fixture options".as_ptr(), ptr::null_mut(), 0);
            renderer.options(snapshot, handle);
            imgui::igEnd();
            imgui::igRender();
            assert!((*imgui::igGetDrawData()).TotalVtxCount > 0);
        }
        widgets::TRACE.lock().unwrap().clone()
    }

    fn export(&self, path: &std::path::Path) {
        let mut bytes = b"MUI1".to_vec();
        let integer =
            |bytes: &mut Vec<u8>, value: u32| bytes.extend_from_slice(&value.to_le_bytes());
        // SAFETY: The fixture owns the completed frame, font atlas, and all draw-list allocations.
        // No ImGui call mutates those buffers while they are copied into this test-only artifact.
        unsafe {
            let data = &*imgui::igGetDrawData();
            integer(&mut bytes, data.DisplaySize.x as u32);
            integer(&mut bytes, data.DisplaySize.y as u32);
            let (mut pixels, mut width, mut height, mut channels) = (ptr::null_mut(), 0, 0, 0);
            imgui::ImFontAtlas_GetTexDataAsRGBA32(
                (*imgui::igGetIO()).Fonts,
                &mut pixels,
                &mut width,
                &mut height,
                &mut channels,
            );
            assert_eq!(channels, 4);
            integer(&mut bytes, width as u32);
            integer(&mut bytes, height as u32);
            bytes.extend_from_slice(std::slice::from_raw_parts(
                pixels,
                (width * height * channels) as usize,
            ));
            integer(&mut bytes, data.CmdListsCount as u32);
            for index in 0..data.CmdListsCount as usize {
                let list = &**data.CmdLists.add(index);
                integer(&mut bytes, list.VtxBuffer.Size as u32);
                integer(&mut bytes, list.IdxBuffer.Size as u32);
                integer(&mut bytes, list.CmdBuffer.Size as u32);
                for index in 0..list.VtxBuffer.Size as usize {
                    let vertex = &*list.VtxBuffer.Data.add(index);
                    for value in [vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y] {
                        bytes.extend_from_slice(&value.to_le_bytes());
                    }
                    integer(&mut bytes, vertex.col);
                }
                for index in 0..list.IdxBuffer.Size as usize {
                    integer(&mut bytes, u32::from(*list.IdxBuffer.Data.add(index)));
                }
                for index in 0..list.CmdBuffer.Size as usize {
                    let command = &*list.CmdBuffer.Data.add(index);
                    assert!(command.UserCallback.is_none());
                    for value in [
                        command.ClipRect.x,
                        command.ClipRect.y,
                        command.ClipRect.z,
                        command.ClipRect.w,
                    ] {
                        bytes.extend_from_slice(&value.to_le_bytes());
                    }
                    for value in [command.VtxOffset, command.IdxOffset, command.ElemCount] {
                        integer(&mut bytes, value);
                    }
                }
            }
        }
        std::fs::write(path, bytes).unwrap();
    }
}
impl Drop for Gui {
    fn drop(&mut self) {
        // SAFETY: All frame work has ended and this fixture exclusively owns the context.
        unsafe {
            imgui::igDestroyContext(self.context);
        }
    }
}

#[test]
fn mouse_toggle_only_queues_until_owner_tick_and_preserves_other_settings() {
    let fixture = Fixture::new();
    let mut runtime = fixture.runtime();
    let handle = runtime.handle();
    let snapshot = handle.snapshot();
    let mut renderer = Renderer::new(&snapshot);
    let gui = Gui::new();
    gui.options(&mut renderer, &snapshot, &handle, [-1.0, -1.0], false);
    let trace = gui.options(&mut renderer, &snapshot, &handle, [-1.0, -1.0], false);
    let rect = trace
        .iter()
        .find(|(title, _)| title == "dps.report")
        .unwrap()
        .1;
    let point = [rect[0] + 5.0, rect[1] + 5.0];
    gui.options(&mut renderer, &snapshot, &handle, point, true);
    gui.options(&mut renderer, &snapshot, &handle, point, false);
    assert!(!fixture.0.join("addon/settings.json").exists());
    runtime
        .tick(&CancellationSource::default().token())
        .unwrap();
    let applied = handle.snapshot();
    assert!(!applied.options.configuration.settings.dps_report.enabled);
    assert_eq!(
        applied.options.configuration.settings.wingman,
        snapshot.options.configuration.settings.wingman
    );
    assert!(fixture.0.join("addon/settings.json").exists());
}

#[test]
fn awaiting_authorization_renders_only_mask_and_warning() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let handle = runtime.handle();
    let mut snapshot = handle.snapshot();
    snapshot.options.twitch.state = TwitchConnectionState::AwaitingUser;
    snapshot.options.twitch.user_code = Some("SYNTHETIC-CODE".into());
    snapshot.options.twitch.verification_uri =
        Some("https://www.twitch.tv/activate?device=synthetic-private".into());
    let mut renderer = Renderer::new(&snapshot);
    let gui = Gui::new();
    let trace = gui.options(&mut renderer, &snapshot, &handle, [-1.0, -1.0], false);
    let text = trace
        .iter()
        .map(|(text, _)| text.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    assert!(text.contains("********"));
    assert!(text.contains("visible in your browser"));
    assert!(text.contains("Open Twitch authorization"));
    assert!(!text.contains("SYNTHETIC-CODE"));
    assert!(!text.contains("synthetic-private"));
}

#[test]
fn endpoint_and_ordinary_drafts_survive_unrelated_snapshots() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let mut snapshot = runtime.handle().snapshot();
    let mut renderer = Renderer::new(&snapshot);
    renderer.donbot_base.set("https://synthetic.invalid");
    renderer.endpoint_dirty = true;
    renderer.log_directory.set("C:/synthetic/edited");
    renderer.dirty = true;
    snapshot.options.configuration.settings.wingman.enabled = false;
    snapshot
        .options
        .configuration
        .settings
        .general
        .window_visible = false;
    renderer.sync(&snapshot);
    assert_eq!(renderer.donbot_base.text(), "https://synthetic.invalid");
    assert_eq!(
        renderer.draft(&snapshot).general.log_directory,
        "C:/synthetic/edited"
    );
    assert!(!renderer.draft(&snapshot).general.window_visible);
}

#[test]
fn acknowledged_save_preserves_edits_made_while_command_was_pending() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let mut snapshot = runtime.handle().snapshot();
    let mut renderer = Renderer::new(&snapshot);
    renderer.log_directory.set("C:/synthetic/first");
    renderer.dirty = true;
    renderer.submitted = Some(renderer.draft(&snapshot));
    snapshot
        .options
        .configuration
        .settings
        .general
        .log_directory = "C:/synthetic/first".into();
    renderer.log_directory.set("C:/synthetic/second");
    renderer.sync(&snapshot);
    assert!(renderer.dirty);
    assert!(renderer.submitted.is_none());
    assert_eq!(renderer.log_directory.text(), "C:/synthetic/second");
    renderer.submitted = Some(renderer.draft(&snapshot));
    snapshot
        .options
        .configuration
        .settings
        .general
        .log_directory = "C:/synthetic/second".into();
    renderer.sync(&snapshot);
    assert!(!renderer.dirty);
}

#[test]
fn verification_hides_and_wipes_the_candidate_key() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let mut snapshot = runtime.handle().snapshot();
    let mut renderer = Renderer::new(&snapshot);
    renderer.donbot_key.set("synthetic-key");
    snapshot.options.donbot.state = DonBotConfigurationState::Verifying;
    renderer.sync(&snapshot);
    assert_eq!(renderer.donbot_key.text(), "");
}

#[test]
fn unverified_candidate_key_survives_frames_until_submission() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let snapshot = runtime.handle().snapshot();
    let mut renderer = Renderer::new(&snapshot);
    renderer.donbot_key.set("synthetic-key");
    renderer.sync(&snapshot);
    assert_eq!(renderer.donbot_key.text(), "synthetic-key");
}

#[test]
fn verified_options_replace_key_entry_with_disconnect_and_server_controls() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let handle = runtime.handle();
    let snapshot = completed_snapshot(&handle);
    let mut renderer = Renderer::new(&snapshot);
    renderer.donbot_key.set("synthetic-key");
    let gui = Gui::new();
    let trace = gui.options(&mut renderer, &snapshot, &handle, [-1.0, -1.0], false);
    assert_eq!(renderer.donbot_key.text(), "");
    assert!(trace.iter().any(|(text, _)| text == "Disconnect DonBot"));
    assert!(
        trace
            .iter()
            .any(|(text, _)| text == "Refresh DonBot servers")
    );
    assert!(!trace.iter().any(|(text, _)| text == "Verify DonBot"));
}

fn completed_snapshot(handle: &RuntimeHandle) -> RuntimeSnapshot {
    use manny_core::domain::*;
    use std::time::{Instant, SystemTime};
    let mut snapshot = handle.snapshot();
    let now = SystemTime::now();
    let mut records = manny_core::config::decode_history(
        include_bytes!("../../../tests/fixtures/history-v1.json"),
        10_000,
    )
    .unwrap();
    let record = &mut records[0];
    record.file.last_write_time = now;
    record.detected_at = now;
    snapshot.jobs = vec![UploadJobSnapshot {
        id: UploadJobId::new(1).unwrap(),
        record: record.clone(),
    }];
    snapshot.selection = SessionLogSelection::new(now, Instant::now());
    snapshot.root_available = true;
    let settings = &mut snapshot.options.configuration.settings;
    settings.donbot.enabled = true;
    settings.donbot.selected_guild_id = "123".into();
    settings.donbot.discord_delivery_enabled = true;
    settings.twitch.enabled = true;
    snapshot.options.twitch.state = TwitchConnectionState::Connected;
    snapshot.options.twitch.login = Some("synthetic_broadcaster".into());
    let bot = &mut snapshot.options.donbot;
    bot.state = DonBotConfigurationState::Verified;
    bot.account_name = Some("Synthetic.1234".into());
    bot.selected_guild_id = "123".into();
    bot.discord_summary_delivery_v1 = true;
    bot.discord_aggregate_delivery_v1 = true;
    bot.guilds = vec![DonBotGuild {
        guild_id: "123".into(),
        guild_name: "Fixture guild".into(),
        discord_delivery: DonBotDiscordDeliveryPolicy {
            enabled: true,
            defaults_available: true,
            pve_summary: true,
            aggregate_enabled: true,
            aggregate_defaults_available: true,
            max_aggregate_fight_logs: 100,
            ..Default::default()
        },
    }];
    snapshot
}

#[test]
fn completed_rows_render_without_paths_and_keep_replay_controls() {
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let handle = runtime.handle();
    let snapshot = completed_snapshot(&handle);
    let mut renderer = Renderer::new(&snapshot);
    let _gui = Gui::new();
    let mut visible = true;
    // SAFETY: The fixture owns this frame and renderer scopes balance all windows and tables.
    unsafe {
        imgui::igNewFrame();
        renderer.main(&snapshot, &handle, &mut visible);
        imgui::igRender();
        imgui::igNewFrame();
        renderer.main(&snapshot, &handle, &mut visible);
        imgui::igRender();
    }
    let trace = widgets::TRACE.lock().unwrap();
    let text = trace
        .iter()
        .map(|(text, _)| text.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    assert!(text.contains("Vale Guardian"));
    assert!(text.contains("Reupload"));
    assert!(text.contains("Rechat"));
    assert!(!text.contains("C:/logs"));
    assert!(!text.contains(".zevtc"));
    assert!(!fixture.0.join("addon/settings.json").exists());
}

#[test]
#[ignore = "writes synthetic draw data for explicit visual review"]
fn export_renderer_preview() {
    let directory = PathBuf::from(
        std::env::var_os("MANNY_UI_PREVIEW").expect("set the preview output directory"),
    );
    assert!(directory.is_dir());
    let fixture = Fixture::new();
    let runtime = fixture.runtime();
    let handle = runtime.handle();
    let snapshot = completed_snapshot(&handle);
    let mut renderer = Renderer::new(&snapshot);
    let gui = Gui::new();
    for _ in 0..2 {
        gui.options(&mut renderer, &snapshot, &handle, [-1.0, -1.0], false);
    }
    gui.export(&directory.join("options.mui"));
    let mut visible = true;
    // SAFETY: The fixture owns both completed frames and all renderer calls remain inside them.
    unsafe {
        for _ in 0..2 {
            imgui::igNewFrame();
            renderer.main(&snapshot, &handle, &mut visible);
            imgui::igRender();
        }
    }
    gui.export(&directory.join("recent.mui"));
}
