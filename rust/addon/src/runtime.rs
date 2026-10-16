use std::ffi::{CString, c_char};
use std::panic::{AssertUnwindSafe, PanicHookInfo, catch_unwind};
use std::ptr;
use std::sync::{Arc, Mutex};

use manny_core::addon::CallbackGate;
use manny_core::{application::*, ui::*};
use nexus::AddonApi;
use nexus::addon::{AddonDefinition, AddonFlags, AddonVersion, UpdateProvider};
use nexus::gui::RenderType;
use nexus::imgui::sys as imgui;

use crate::host::{self, Attachment, Host};
use crate::icons;
use crate::{composition, ui::Renderer};

#[cfg(test)]
mod tests;

static OWNER: Mutex<()> = Mutex::new(());
static STATE: Mutex<Option<RuntimeState>> = Mutex::new(None);
static GATE: CallbackGate = CallbackGate::new();

type PanicHook = Box<dyn Fn(&PanicHookInfo<'_>) + Send + Sync + 'static>;

struct Presentation {
    renderer: Renderer,
    visible: bool,
    pending_visibility: Option<u64>,
}
impl Presentation {
    fn synchronize_visibility(&mut self, snapshot: &RuntimeSnapshot) {
        let options = &snapshot.options;
        if let Some(revision) = self.pending_visibility {
            if options.revision <= revision || options.pending_commands != 0 {
                return;
            }
            self.pending_visibility = None;
        }
        self.visible = options.configuration.settings.general.window_visible;
    }
    fn set_visible(&mut self, visible: bool, snapshot: &RuntimeSnapshot, handle: &RuntimeHandle) {
        if handle
            .submit_options(NexusOptionsCommand::SetWindowVisible(visible))
            .is_ok()
        {
            self.visible = visible;
            self.pending_visibility = Some(snapshot.options.revision);
        }
    }
}

struct Shortcut {
    status: QuickAccessStatus,
    tooltip: CString,
    registered: bool,
}
impl Shortcut {
    fn new(snapshot: &RuntimeSnapshot) -> Self {
        let status = build_quick_access_status(
            &snapshot.options.configuration.settings,
            &snapshot.options.donbot,
        );
        let tooltip = CString::new(status.tooltip.as_str()).expect("status contains no NUL");
        Self {
            status,
            tooltip,
            registered: false,
        }
    }
    fn texture(&self) -> &std::ffi::CStr {
        match self.status.tint {
            QuickAccessTint::Idle => host::IDLE,
            QuickAccessTint::Upload => host::NORMAL,
            QuickAccessTint::Twitch => host::TWITCH,
        }
    }
}

struct Runtime {
    handle: RuntimeHandle,
    presentation: Mutex<Presentation>,
    shortcut: Mutex<Shortcut>,
    host: Host,
}

#[derive(Clone, Copy)]
enum Resource {
    Main,
    Options,
    Input,
    Shortcut,
}

struct RuntimeState {
    host: Host,
    runtime: Arc<Runtime>,
    owner: ApplicationRuntimeThread,
    resources: Vec<Resource>,
    previous_hook: Option<PanicHook>,
}

static DEFINITION: AddonDefinition = AddonDefinition {
    signature: 0xB2D15965_u32 as i32,
    api_version: AddonApi::VERSION,
    name: c"MannyUploader".as_ptr(),
    version: AddonVersion {
        major: 0,
        minor: 1,
        build: 0,
        revision: 0,
    },
    author: c"Logan".as_ptr(),
    description: DESCRIPTION.as_ptr(),
    load,
    unload: Some(unload),
    flags: AddonFlags::None,
    provider: UpdateProvider::GitHub,
    update_link: c"https://github.com/LoganWal/GW2-MannyUploader".as_ptr(),
};

#[cfg(not(feature = "lifecycle-test"))]
const DESCRIPTION: &std::ffi::CStr =
    c"Upload arcdps logs and share report links. Rust development build.";
#[cfg(feature = "lifecycle-test")]
const DESCRIPTION: &std::ffi::CStr =
    c"TEST ONLY. Synthetic blocking HTTP. Never install this DLL in Guild Wars 2.";

#[cfg(feature = "lifecycle-test")]
pub(crate) fn lifecycle_test_jobs() -> u64 {
    let Ok(state) = STATE.try_lock() else {
        return u64::MAX;
    };
    let Some(state) = state.as_ref() else {
        return 0;
    };
    let jobs = state.runtime.handle.snapshot().jobs;
    let active = jobs
        .iter()
        .filter(|job| job.record.providers[0].state == manny_core::domain::ProviderState::Active)
        .count();
    (jobs.len() as u64).min(0xffff) | ((active as u64).min(0xffff) << 16)
}

#[unsafe(no_mangle)]
pub extern "system" fn GetAddonDef() -> *const AddonDefinition {
    &DEFINITION
}

unsafe extern "C-unwind" fn load(api: *const AddonApi) {
    let _owner = OWNER.lock().unwrap_or_else(|error| error.into_inner());
    if STATE
        .lock()
        .unwrap_or_else(|error| error.into_inner())
        .is_some()
    {
        return;
    }
    let gate = &GATE;
    // Keep ownership outside the unwind boundary until construction and registration finish.
    // set_hook cannot run from a destructor on a panicking thread.
    let mut previous_hook = None;
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Nexus supplies a readable API allocation. Attachment validates all used slots.
        let attachment = unsafe { Attachment::read(api) }?;
        previous_hook = Some(std::panic::take_hook());
        // Panic payloads can contain credentials. Only static host diagnostics
        // are emitted by boundaries. Restore the previous hook before unloading this module.
        std::panic::set_hook(Box::new(|_| {}));
        let application = composition::compose(
            &attachment.paths[0],
            &attachment.paths[1],
            composition::PACKAGED_TWITCH_CLIENT_ID.into(),
        )
        .map_err(|_| {
            attachment.host.report_failure();
        })?;
        let handle = application.handle();
        let snapshot = handle.snapshot();
        let owner = ApplicationRuntimeThread::start(application).map_err(|_| {
            attachment.host.report_failure();
        })?;
        let mut state = RuntimeState {
            host: attachment.host,
            runtime: Arc::new(Runtime {
                handle,
                presentation: Mutex::new(Presentation {
                    renderer: Renderer::new(&snapshot),
                    visible: snapshot
                        .options
                        .configuration
                        .settings
                        .general
                        .window_visible,
                    pending_visibility: None,
                }),
                shortcut: Mutex::new(Shortcut::new(&snapshot)),
                host: attachment.host,
            }),
            owner,
            resources: Vec::with_capacity(4),
            previous_hook: None,
        };
        // SAFETY: Validated host context and allocators use the pinned ImGui 1.80 ABI. Calls are
        // serialized with unload, and admission stays closed until registration completes.
        unsafe {
            imgui::igSetCurrentContext(attachment.context);
            imgui::igSetAllocatorFunctions(
                Some(attachment.allocate),
                Some(attachment.free),
                ptr::null_mut(),
            );
        }
        let registered = catch_unwind(AssertUnwindSafe(|| {
            // Record before calling the host so a callback panic can roll back partial registration.
            // SAFETY: Validated API functions receive static identifiers and DLL-owned callbacks.
            unsafe {
                state.resources.push(Resource::Main);
                (state.host.register_render)(RenderType::Render, render_main);
                state.resources.push(Resource::Options);
                (state.host.register_render)(RenderType::OptionsRender, render_options);
                state.resources.push(Resource::Input);
                (state.host.register_input)(host::INPUT.as_ptr(), input, c"ALT+SHIFT+M".as_ptr());
            }
        }));
        if registered.is_err() {
            state.host.report_failure();
            release(state);
            return Err(());
        }
        let optional = catch_unwind(AssertUnwindSafe(|| {
            // SAFETY: Nexus consumes the static PNG bytes synchronously. No texture pointer or
            // asynchronous callback is retained, including when GPU texture creation is pending.
            unsafe {
                for (identifier, bytes) in [
                    (host::NORMAL, icons::NORMAL),
                    (host::IDLE, icons::IDLE),
                    (host::TWITCH, icons::TWITCH),
                ] {
                    (state.host.create_texture)(
                        identifier.as_ptr(),
                        bytes.as_ptr().cast(),
                        bytes.len(),
                    );
                }
                state.resources.push(Resource::Shortcut);
                let mut shortcut = state
                    .runtime
                    .shortcut
                    .lock()
                    .unwrap_or_else(|error| error.into_inner());
                shortcut.registered = true;
                (state.host.add_shortcut)(
                    host::SHORTCUT.as_ptr(),
                    shortcut.texture().as_ptr(),
                    shortcut.texture().as_ptr(),
                    host::INPUT.as_ptr(),
                    shortcut.tooltip.as_ptr(),
                );
            }
        }));
        if optional.is_err() {
            state.host.report_failure();
        }
        *STATE.lock().unwrap_or_else(|error| error.into_inner()) = Some(state);
        assert!(gate.open(), "Callback gate must be drained before loading");
        Ok::<(), ()>(())
    }));
    if !matches!(outcome, Ok(Ok(()))) {
        shutdown();
    } else if let Some(state) = STATE
        .lock()
        .unwrap_or_else(|error| error.into_inner())
        .as_mut()
    {
        state.previous_hook = previous_hook.take();
    }
    if let Some(hook) = previous_hook {
        std::panic::set_hook(hook);
    }
}

unsafe extern "C-unwind" fn unload() {
    let _owner = OWNER.lock().unwrap_or_else(|error| error.into_inner());
    // All project-owned callback and cleanup operations contain unwinding locally.
    let _ = catch_unwind(AssertUnwindSafe(shutdown));
}

fn shutdown() {
    GATE.close();
    let state = STATE
        .lock()
        .unwrap_or_else(|error| error.into_inner())
        .take();
    if let Some(state) = state {
        release(state);
    }
}

fn release(mut state: RuntimeState) {
    GATE.close();
    for resource in state.resources.drain(..).rev() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            // SAFETY: These are the exact resources admitted during load. Closing the gate makes
            // synchronous callbacks during deregistration no-ops. The host is still alive.
            unsafe {
                match resource {
                    Resource::Shortcut => {
                        // An admitted render may be updating this resource while unload starts.
                        // Serialize its last update with removal and prevent later re-registration.
                        let mut shortcut = state
                            .runtime
                            .shortcut
                            .lock()
                            .unwrap_or_else(|error| error.into_inner());
                        shortcut.registered = false;
                        (state.host.remove_shortcut)(host::SHORTCUT.as_ptr());
                    }
                    Resource::Input => (state.host.deregister_input)(host::INPUT.as_ptr()),
                    Resource::Options => (state.host.deregister_render)(render_options),
                    Resource::Main => (state.host.deregister_render)(render_main),
                }
            }
        }));
        if result.is_err() {
            state.host.report_failure();
        }
    }
    GATE.wait();
    state.owner.shutdown();
    drop(state.runtime);
    // SAFETY: Every admitted callback has returned. Clear borrowed host state without destroying
    // its context or textures. A later load installs fresh allocator functions before any UI work.
    unsafe {
        imgui::igSetCurrentContext(ptr::null_mut());
        imgui::igSetAllocatorFunctions(None, None, ptr::null_mut());
    }
    if let Some(hook) = state.previous_hook.take() {
        std::panic::set_hook(hook);
    }
}

fn callback(action: impl FnOnce(&Runtime)) {
    let Some(_admission) = GATE.enter() else {
        return;
    };
    let (runtime, host) = {
        let Ok(state) = STATE.try_lock() else {
            return;
        };
        let Some(state) = state.as_ref() else {
            return;
        };
        (Arc::clone(&state.runtime), state.host)
    };
    // Release the state lock before UI code or host calls can synchronously re-enter a callback.
    // The admission guard keeps runtime and host use within the unload barrier.
    if catch_unwind(AssertUnwindSafe(|| action(&runtime))).is_err() {
        host.report_failure();
    }
}

extern "C-unwind" fn render_main() {
    callback(|runtime| {
        let snapshot = runtime.handle.snapshot();
        runtime.refresh_shortcut(&snapshot);
        let Ok(mut presentation) = runtime.presentation.try_lock() else {
            return;
        };
        presentation.synchronize_visibility(&snapshot);
        let mut visible = presentation.visible;
        // SAFETY: Nexus calls rendering inside its active ImGui frame on the render thread.
        unsafe {
            presentation
                .renderer
                .main(&snapshot, &runtime.handle, &mut visible);
        }
        if visible != presentation.visible {
            presentation.set_visible(visible, &snapshot, &runtime.handle);
        }
    });
}

extern "C-unwind" fn render_options() {
    callback(|runtime| {
        let snapshot = runtime.handle.snapshot();
        let Ok(mut presentation) = runtime.presentation.try_lock() else {
            return;
        };
        // SAFETY: Nexus invokes the options callback inside an active ImGui frame.
        unsafe {
            presentation.renderer.options(&snapshot, &runtime.handle);
        }
    });
}

extern "C-unwind" fn input(_identifier: *const c_char, is_release: bool) {
    if !is_release {
        callback(|runtime| {
            let snapshot = runtime.handle.snapshot();
            let Ok(mut presentation) = runtime.presentation.try_lock() else {
                return;
            };
            presentation.synchronize_visibility(&snapshot);
            let visible = !presentation.visible;
            presentation.set_visible(visible, &snapshot, &runtime.handle);
        });
    }
}

impl Runtime {
    fn refresh_shortcut(&self, snapshot: &RuntimeSnapshot) {
        let Ok(mut shortcut) = self.shortcut.try_lock() else {
            return;
        };
        let status = build_quick_access_status(
            &snapshot.options.configuration.settings,
            &snapshot.options.donbot,
        );
        if !shortcut.registered || shortcut.status == status {
            return;
        }
        let tooltip = CString::new(status.tooltip.as_str()).expect("status contains no NUL");
        // SAFETY: The callback gate keeps the validated host alive. Remove/add is balanced and
        // the owned tooltip remains live until the next replacement or final deregistration.
        unsafe {
            (self.host.remove_shortcut)(host::SHORTCUT.as_ptr());
            shortcut.status = status;
            shortcut.tooltip = tooltip;
            (self.host.add_shortcut)(
                host::SHORTCUT.as_ptr(),
                shortcut.texture().as_ptr(),
                shortcut.texture().as_ptr(),
                host::INPUT.as_ptr(),
                shortcut.tooltip.as_ptr(),
            );
        }
    }
}
