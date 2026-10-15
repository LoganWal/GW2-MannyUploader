use std::ffi::c_char;
use std::panic::{AssertUnwindSafe, PanicHookInfo, catch_unwind};
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use manny_core::addon::CallbackGate;
use nexus::AddonApi;
use nexus::addon::{AddonDefinition, AddonFlags, AddonVersion, UpdateProvider};
use nexus::gui::RenderType;
use nexus::imgui::sys as imgui;

use crate::host::{self, Attachment, Host};
use crate::icons;

#[cfg(test)]
mod tests;

static OWNER: Mutex<()> = Mutex::new(());
static STATE: Mutex<Option<RuntimeState>> = Mutex::new(None);
static GATE: CallbackGate = CallbackGate::new();

type PanicHook = Box<dyn Fn(&PanicHookInfo<'_>) + Send + Sync + 'static>;

struct Runtime {
    _paths: [PathBuf; 2],
    visible: AtomicBool,
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
    description: c"Development lifecycle shell. Uploads are not available yet.".as_ptr(),
    load,
    unload: Some(unload),
    flags: AddonFlags::None,
    provider: UpdateProvider::GitHub,
    update_link: c"https://github.com/LoganWal/GW2-MannyUploader".as_ptr(),
};

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
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        // SAFETY: Nexus supplies a readable API allocation. Attachment validates all used slots.
        let attachment = unsafe { Attachment::read(api) }?;
        let previous_hook = std::panic::take_hook();
        // Panic payloads can contain credentials later in the migration. Only static host diagnostics
        // are emitted by boundaries. Restore the previous hook before unloading this module.
        std::panic::set_hook(Box::new(|_| {}));
        let mut state = RuntimeState {
            host: attachment.host,
            runtime: Arc::new(Runtime {
                _paths: attachment.paths,
                visible: AtomicBool::new(false),
            }),
            resources: Vec::with_capacity(4),
            previous_hook: Some(previous_hook),
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
                (state.host.add_shortcut)(
                    host::SHORTCUT.as_ptr(),
                    host::IDLE.as_ptr(),
                    host::IDLE.as_ptr(),
                    host::INPUT.as_ptr(),
                    c"View MannyUploader list".as_ptr(),
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
                    Resource::Shortcut => (state.host.remove_shortcut)(host::SHORTCUT.as_ptr()),
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
        if runtime.visible.load(Ordering::Acquire) {
            // SAFETY: Nexus calls rendering inside its active ImGui frame on the render thread.
            unsafe {
                if imgui::igBegin(c"MannyUploader".as_ptr(), ptr::null_mut(), 0) {
                    imgui::igTextUnformatted(
                        c"Development build. Uploads are not available yet.".as_ptr(),
                        ptr::null(),
                    );
                }
                imgui::igEnd();
            }
        }
    });
}

extern "C-unwind" fn render_options() {
    callback(|_| {
        // SAFETY: Nexus invokes the options callback inside an active ImGui frame.
        unsafe {
            imgui::igTextUnformatted(
                c"MannyUploader development build. Uploads are not available yet.".as_ptr(),
                ptr::null(),
            );
        }
    });
}

extern "C-unwind" fn input(_identifier: *const c_char, is_release: bool) {
    if !is_release {
        callback(|runtime| {
            runtime.visible.fetch_xor(true, Ordering::AcqRel);
        });
    }
}
