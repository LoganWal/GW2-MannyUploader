use super::*;
use std::ffi::{CStr, c_void};
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

static EVENTS: Mutex<Vec<&str>> = Mutex::new(Vec::new());
static FAIL_OPTIONS: AtomicBool = AtomicBool::new(false);
static FAIL_TEXTURE: AtomicBool = AtomicBool::new(false);
static HOOK_CALLS: AtomicUsize = AtomicUsize::new(0);
static TEST_PATHS: Mutex<Option<[CString; 2]>> = Mutex::new(None);

#[test]
fn nexus_version_uses_date_and_build() {
    let expected: AddonVersion = include!(concat!(env!("OUT_DIR"), "/addon_version.rs"));
    assert_eq!(DEFINITION.version.major, expected.major);
    assert_eq!(DEFINITION.version.minor, expected.minor);
    assert_eq!(DEFINITION.version.build, expected.build);
    assert_eq!(DEFINITION.version.revision, expected.revision);
    assert!(expected.major >= 2026);
    assert!((1..=31).contains(&expected.minor));
    assert!((1..=12).contains(&expected.build));
    assert!(expected.revision > 0);
}

unsafe extern "C-unwind" fn register(kind: RenderType, callback: nexus::gui::RawGuiRender) {
    EVENTS.lock().unwrap().push(if kind == RenderType::Render {
        "main"
    } else {
        "options"
    });
    // Invoked before a frame exists. A registration callback must be rejected by the gate.
    callback();
    if kind == RenderType::OptionsRender && FAIL_OPTIONS.load(Ordering::Relaxed) {
        panic!("synthetic registration failure");
    }
}

unsafe extern "C-unwind" fn deregister(callback: nexus::gui::RawGuiRender) {
    let main = std::ptr::fn_addr_eq(callback, render_main as nexus::gui::RawGuiRender);
    EVENTS.lock().unwrap().push(if main {
        "remove main"
    } else {
        "remove options"
    });
    callback();
}

unsafe extern "C-unwind" fn register_input(
    _: *const c_char,
    callback: nexus::keybind::RawKeybindHandler,
    _: *const c_char,
) {
    EVENTS.lock().unwrap().push("input");
    callback(ptr::null(), false);
}

unsafe extern "C-unwind" fn remove_input(_: *const c_char) {
    EVENTS.lock().unwrap().push("remove input");
}
unsafe extern "C-unwind" fn texture(
    _: *const c_char,
    _: *const c_void,
    _: usize,
) -> *const nexus::texture::Texture {
    if FAIL_TEXTURE.load(Ordering::Relaxed) {
        panic!("synthetic texture failure");
    }
    ptr::null()
}
unsafe extern "C-unwind" fn shortcut(
    _: *const c_char,
    _: *const c_char,
    _: *const c_char,
    _: *const c_char,
    _: *const c_char,
) {
    EVENTS.lock().unwrap().push("shortcut");
}
unsafe extern "C-unwind" fn remove_shortcut(_: *const c_char) {
    EVENTS.lock().unwrap().push("remove shortcut");
}
unsafe extern "C-unwind" fn path() -> *const c_char {
    TEST_PATHS.lock().unwrap().as_ref().unwrap()[0].as_ptr()
}
unsafe extern "C-unwind" fn addon_path(_: *const c_char) -> *const c_char {
    TEST_PATHS.lock().unwrap().as_ref().unwrap()[1].as_ptr()
}
unsafe extern "C-unwind" fn log(_: nexus::log::LogLevel, _: *const c_char, message: *const c_char) {
    assert_eq!(
        // SAFETY: The addon supplies its static diagnostic C string.
        unsafe { CStr::from_ptr(message) },
        c"An addon callback failed"
    );
    EVENTS.lock().unwrap().push("diagnostic");
}

unsafe extern "C" {
    fn malloc(size: usize) -> *mut c_void;
    fn free(memory: *mut c_void);
}

unsafe extern "C" fn allocate(size: usize, _: *mut c_void) -> *mut c_void {
    // SAFETY: Forward the allocator contract unchanged to this test executable's CRT.
    unsafe { malloc(size) }
}
unsafe extern "C" fn deallocate(memory: *mut c_void, _: *mut c_void) {
    // SAFETY: ImGui returns allocations obtained through allocate.
    unsafe { free(memory) }
}

#[test]
fn native_lifecycle_contains_panics_rolls_back_and_can_reload() {
    let _imgui = crate::IMGUI_TEST_LOCK.lock().unwrap();
    let root = std::env::temp_dir().join(format!("manny-native-lifecycle-{}", std::process::id()));
    std::fs::create_dir(&root).unwrap();
    let game = root.join("game");
    let addon = root.join("addon");
    std::fs::create_dir(&game).unwrap();
    std::fs::create_dir(&addon).unwrap();
    *TEST_PATHS.lock().unwrap() = Some([
        CString::new(game.to_str().unwrap()).unwrap(),
        CString::new(addon.to_str().unwrap()).unwrap(),
    ]);
    let mut settings =
        manny_core::domain::Settings::new(game.join("arcdps.cbtlogs").to_str().unwrap().into());
    settings.general.window_visible = false;
    settings.dps_report.enabled = false;
    settings.wingman.enabled = false;
    let settings_bytes = manny_core::config::encode_settings(&settings).unwrap();
    std::fs::write(addon.join("settings.json"), &settings_bytes).unwrap();
    let original_hook = std::panic::take_hook();
    let test_thread = std::thread::current().id();
    std::panic::set_hook(Box::new(move |_| {
        if std::thread::current().id() == test_thread {
            HOOK_CALLS.fetch_add(1, Ordering::Relaxed);
        }
    }));
    // SAFETY: This fixture owns the ImGui context. The API allocation stays MaybeUninit because
    // unused nullable host slots cannot be represented by a valid Rust AddonApi value.
    unsafe {
        imgui::igSetAllocatorFunctions(Some(allocate), Some(deallocate), ptr::null_mut());
        let context = imgui::igCreateContext(ptr::null_mut());
        let mut storage = Box::new(MaybeUninit::<AddonApi>::zeroed());
        let api = storage.as_mut_ptr();
        ptr::addr_of_mut!((*api).imgui_context).write(context);
        ptr::addr_of_mut!((*api).imgui_malloc).write(Some(allocate));
        ptr::addr_of_mut!((*api).imgui_free).write(Some(deallocate));
        ptr::addr_of_mut!((*api).renderer.register).write(register);
        ptr::addr_of_mut!((*api).renderer.deregister).write(deregister);
        ptr::addr_of_mut!((*api).input_binds.register_with_string).write(register_input);
        ptr::addr_of_mut!((*api).input_binds.deregister).write(remove_input);
        ptr::addr_of_mut!((*api).texture.get_or_create_from_memory).write(texture);
        ptr::addr_of_mut!((*api).quick_access.add).write(shortcut);
        ptr::addr_of_mut!((*api).quick_access.remove).write(remove_shortcut);
        ptr::addr_of_mut!((*api).path.get_game_dir).write(path);
        ptr::addr_of_mut!((*api).path.get_addon_dir).write(addon_path);
        ptr::addr_of_mut!((*api).log).write(log);

        std::fs::write(addon.join("settings.json"), b"invalid settings").unwrap();
        load(api);
        assert!(STATE.lock().unwrap().is_none());
        assert_eq!(*EVENTS.lock().unwrap(), ["diagnostic"]);
        let _ = catch_unwind(|| panic!("synthetic construction hook restoration"));
        assert_eq!(HOOK_CALLS.swap(0, Ordering::Relaxed), 1);
        EVENTS.lock().unwrap().clear();
        std::fs::write(addon.join("settings.json"), &settings_bytes).unwrap();

        FAIL_OPTIONS.store(true, Ordering::Relaxed);
        load(api);
        assert_eq!(
            *EVENTS.lock().unwrap(),
            [
                "main",
                "options",
                "diagnostic",
                "remove options",
                "remove main"
            ]
        );
        assert!(STATE.lock().unwrap().is_none());
        assert!(GATE.enter().is_none());
        assert_eq!(HOOK_CALLS.load(Ordering::Relaxed), 0);
        FAIL_OPTIONS.store(false, Ordering::Relaxed);
        EVENTS.lock().unwrap().clear();

        load(api);
        let count = EVENTS.lock().unwrap().len();
        load(api);
        assert_eq!(EVENTS.lock().unwrap().len(), count);
        let runtime = STATE.lock().unwrap().as_ref().unwrap().runtime.clone();
        let mut status = runtime.handle.snapshot();
        status.options.configuration.settings.dps_report.enabled = true;
        runtime.refresh_shortcut(&status);
        assert_eq!(
            runtime.shortcut.lock().unwrap().status.tint,
            QuickAccessTint::Upload
        );
        status.options.configuration.settings.twitch.enabled = true;
        runtime.refresh_shortcut(&status);
        assert_eq!(
            runtime.shortcut.lock().unwrap().status.tint,
            QuickAccessTint::Twitch
        );
        let count = EVENTS.lock().unwrap().len();
        runtime.refresh_shortcut(&status);
        assert_eq!(EVENTS.lock().unwrap().len(), count);
        drop(runtime);
        input(ptr::null(), true);
        assert!(
            !STATE
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .runtime
                .presentation
                .lock()
                .unwrap()
                .visible
        );
        input(ptr::null(), false);
        assert!(
            STATE
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .runtime
                .presentation
                .lock()
                .unwrap()
                .visible
        );
        callback(|_| panic!("synthetic callback failure"));
        assert_eq!(HOOK_CALLS.load(Ordering::Relaxed), 0);
        let handle = STATE
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .runtime
            .handle
            .clone();
        let retained = STATE.lock().unwrap().as_ref().unwrap().runtime.clone();
        unload();
        assert!(handle.snapshot().shutting_down);
        assert!(!retained.shortcut.lock().unwrap().registered);
        let count = EVENTS.lock().unwrap().len();
        let mut late = handle.snapshot();
        late.options.configuration.settings.dps_report.enabled = true;
        retained.refresh_shortcut(&late);
        assert_eq!(EVENTS.lock().unwrap().len(), count);
        drop(retained);
        unload();
        assert_eq!(
            *EVENTS.lock().unwrap(),
            [
                "main",
                "options",
                "input",
                "shortcut",
                "remove shortcut",
                "shortcut",
                "remove shortcut",
                "shortcut",
                "diagnostic",
                "remove shortcut",
                "remove input",
                "remove options",
                "remove main"
            ]
        );
        render_main();
        render_options();
        input(ptr::null(), false);
        assert!(STATE.lock().unwrap().is_none());

        EVENTS.lock().unwrap().clear();
        FAIL_TEXTURE.store(true, Ordering::Relaxed);
        load(api);
        assert!(STATE.lock().unwrap().is_some());
        unload();
        assert_eq!(
            *EVENTS.lock().unwrap(),
            [
                "main",
                "options",
                "input",
                "diagnostic",
                "remove input",
                "remove options",
                "remove main"
            ]
        );
        FAIL_TEXTURE.store(false, Ordering::Relaxed);

        // Restore the fixture's allocators before destroying the context it owns.
        imgui::igSetAllocatorFunctions(Some(allocate), Some(deallocate), ptr::null_mut());
        imgui::igDestroyContext(context);
    }
    let _ = catch_unwind(|| panic!("synthetic hook restoration check"));
    assert_eq!(HOOK_CALLS.load(Ordering::Relaxed), 1);
    std::panic::set_hook(original_hook);
    *TEST_PATHS.lock().unwrap() = None;
    std::fs::remove_dir_all(root).unwrap();
}
