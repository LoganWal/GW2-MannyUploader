use std::ffi::{CStr, c_char};
use std::path::PathBuf;
use std::ptr;

use nexus::{AddonApi, gui, keybind, log, paths, quick_access, texture};

pub const INPUT: &CStr = c"KB_MANNY_UPLOADER_TOGGLE_WINDOW";
pub const SHORTCUT: &CStr = c"QA_GW2_MANNY_UPLOADER";
pub const NORMAL: &CStr = c"TEX_GW2_MANNY_UPLOADER";
pub const IDLE: &CStr = c"TEX_GW2_MANNY_UPLOADER_IDLE";
pub const TWITCH: &CStr = c"TEX_GW2_MANNY_UPLOADER_TWITCH";

#[derive(Clone, Copy)]
pub struct Host {
    pub register_render: gui::RawGuiAddRender,
    pub deregister_render: gui::RawGuiRemRender,
    pub register_input: keybind::RawKeybindRegisterWithString,
    pub deregister_input: keybind::RawKeybindDeregister,
    pub create_texture: texture::RawTextureGetOrCreateFromMemory,
    pub add_shortcut: quick_access::RawQuickAccessAddShortcut,
    pub remove_shortcut: quick_access::RawQuickAccessGeneric,
    pub log: log::RawLog,
}

pub struct Attachment {
    pub host: Host,
    pub context: *mut nexus::imgui::sys::ImGuiContext,
    pub allocate: gui::ImguiMalloc,
    pub free: gui::ImguiFree,
    pub paths: [PathBuf; 2],
}

impl Attachment {
    /// The host must supply a readable API-v6 allocation and valid NUL-terminated path strings.
    pub unsafe fn read(api: *const AddonApi) -> Result<Self, ()> {
        if api.is_null() {
            return Err(());
        }
        // SAFETY: Only individual ABI fields are read. The complete AddonApi is never borrowed or
        // cloned because unused C function and COM pointer slots may legitimately be null.
        unsafe {
            let host = Host {
                register_render: required(ptr::addr_of!((*api).renderer.register))?,
                deregister_render: required(ptr::addr_of!((*api).renderer.deregister))?,
                register_input: required(ptr::addr_of!((*api).input_binds.register_with_string))?,
                deregister_input: required(ptr::addr_of!((*api).input_binds.deregister))?,
                create_texture: required(ptr::addr_of!((*api).texture.get_or_create_from_memory))?,
                add_shortcut: required(ptr::addr_of!((*api).quick_access.add))?,
                remove_shortcut: required(ptr::addr_of!((*api).quick_access.remove))?,
                log: required(ptr::addr_of!((*api).log))?,
            };
            let context = ptr::addr_of!((*api).imgui_context).read();
            let allocate = ptr::addr_of!((*api).imgui_malloc).read().ok_or(())?;
            let free = ptr::addr_of!((*api).imgui_free).read().ok_or(())?;
            let game: paths::RawGetGameDir = required(ptr::addr_of!((*api).path.get_game_dir))?;
            let addon: paths::RawGetAddonDir = required(ptr::addr_of!((*api).path.get_addon_dir))?;
            if context.is_null() {
                return Err(());
            }
            let paths = [
                owned_path(game())?,
                owned_path(addon(c"GW2MannyUploader".as_ptr()))?,
            ];
            Ok(Self {
                host,
                context,
                allocate,
                free,
                paths,
            })
        }
    }
}

/// T must be a function pointer, whose nullable C representation is Option<T>.
unsafe fn required<T: Copy>(slot: *const T) -> Result<T, ()> {
    // SAFETY: All callers provide aligned function-pointer slots within a readable Nexus API.
    unsafe { slot.cast::<Option<T>>().read().ok_or(()) }
}

unsafe fn owned_path(path: *const c_char) -> Result<PathBuf, ()> {
    if path.is_null() {
        return Err(());
    }
    // SAFETY: Nexus owns this NUL-terminated string for the duration of the load callback.
    let path = unsafe { CStr::from_ptr(path) }.to_str().map_err(|_| ())?;
    if path.is_empty() || path.len() > 32_768 {
        return Err(());
    }
    Ok(PathBuf::from(path))
}

impl Host {
    pub fn report_failure(self) {
        let _ = std::panic::catch_unwind(|| {
            // SAFETY: Host function pointers are validated at load and used before unload returns.
            unsafe {
                (self.log)(
                    log::LogLevel::Critical,
                    c"GW2 Manny Uploader".as_ptr(),
                    c"An addon callback failed".as_ptr(),
                )
            };
        });
    }
}
