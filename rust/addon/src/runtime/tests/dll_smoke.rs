use super::*;
use std::ffi::c_void;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryW(path: *const u16) -> *mut c_void;
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
}

struct Library(*mut c_void);
impl Library {
    fn open(path: &Path) -> Self {
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain([0]).collect();
        // SAFETY: The path is NUL terminated and the module stays loaded until Drop.
        let module = unsafe { LoadLibraryW(wide.as_ptr()) };
        assert!(!module.is_null(), "could not load the Rust addon DLL");
        Self(module)
    }
    fn symbol(&self, name: &CStr) -> *mut c_void {
        // SAFETY: The loaded module remains owned by this value and name is NUL terminated.
        unsafe { GetProcAddress(self.0, name.as_ptr()) }
    }
}
impl Drop for Library {
    fn drop(&mut self) {
        // SAFETY: This fixture owns one LoadLibraryW reference and releases it after addon unload.
        let released = unsafe { FreeLibrary(self.0) };
        assert_ne!(released, 0, "could not unload the addon DLL");
    }
}

fn state_probe(module: &Library, name: &CStr) -> Option<unsafe extern "system" fn() -> u64> {
    let address = module.symbol(name);
    if address.is_null() {
        return None;
    }
    // SAFETY: Lifecycle-test DLLs export this symbol with the declared function signature.
    Some(unsafe { std::mem::transmute::<*mut c_void, unsafe extern "system" fn() -> u64>(address) })
}

fn stage_archives(folder: &Path, cycle: usize, jobs: unsafe extern "system" fn() -> u64) -> bool {
    let archive = crate::composition::tests::synthetic_archive();
    // SAFETY: The lifecycle-test DLL remains loaded while this function pointer is called.
    let existing = unsafe { jobs() } & 0xffff;
    for index in 0..3 {
        let file = folder.join(format!("fixture-{cycle}-{index}.zevtc"));
        std::fs::write(&file, &archive).unwrap();
        // Keep fixture timestamps past the startup cutoff on coarse Windows filesystems.
        let modified = SystemTime::now() + Duration::from_secs(2);
        std::fs::OpenOptions::new()
            .write(true)
            .open(&file)
            .unwrap()
            .set_times(std::fs::FileTimes::new().set_modified(modified))
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(10);
        while Instant::now() < deadline {
            // SAFETY: The lifecycle-test DLL remains loaded while this function pointer is called.
            if unsafe { jobs() } & 0xffff > existing + index {
                break;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        // SAFETY: The lifecycle-test DLL remains loaded while this function pointer is called.
        if unsafe { jobs() } & 0xffff <= existing + index {
            return false;
        }
    }
    true
}

fn wait_for_active(
    state: unsafe extern "system" fn() -> u64,
    jobs: unsafe extern "system" fn() -> u64,
) -> (bool, u64, u64) {
    let deadline = Instant::now() + Duration::from_secs(10);
    let (mut status, mut job_status) = (0, 0);
    while Instant::now() < deadline {
        // SAFETY: The lifecycle-test DLL remains loaded while these function pointers are called.
        (status, job_status) = unsafe { (state(), jobs()) };
        if status >> 63 != 0 {
            return (false, status, job_status);
        }
        if (status >> 32) & 0xffff == 2
            && (status >> 48) & 0x7fff == 1
            && (job_status >> 16) & 0xffff == 3
        {
            return (true, status, job_status);
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    (false, status, job_status)
}

/// Exercises the real DLL separately from the in-process lifecycle unit test.
#[test]
#[ignore = "requires MANNY_SMOKE_DLL from a built Windows addon"]
fn real_dll_smoke() {
    let _guard = crate::IMGUI_TEST_LOCK.lock().unwrap();
    let dll = std::env::var_os("MANNY_SMOKE_DLL").expect("MANNY_SMOKE_DLL is required");
    let active = std::env::var("MANNY_SMOKE_ACTIVE").as_deref() == Ok("1");
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("manny-dll-smoke-{}-{stamp}", std::process::id()));
    let game = root.join("Guild Wars 2");
    let addon = game.join("addons").join("GW2MannyUploader");
    let logs = game.join("arcdps.cbtlogs");
    std::fs::create_dir_all(&addon).unwrap();
    if active {
        std::fs::create_dir_all(&logs).unwrap();
    }
    *TEST_PATHS.lock().unwrap() = Some([
        CString::new(game.to_str().unwrap()).unwrap(),
        CString::new(addon.to_str().unwrap()).unwrap(),
    ]);

    let mut settings = manny_core::domain::Settings::new(logs.to_str().unwrap().into());
    settings.general.window_visible = false;
    settings.general.poll_interval_ms = 250;
    settings.general.parallel_uploads_per_provider = 2;
    settings.dps_report.enabled = active;
    settings.wingman.enabled = false;
    std::fs::write(
        addon.join("settings.json"),
        manny_core::config::encode_settings(&settings).unwrap(),
    )
    .unwrap();

    // SAFETY: This fixture owns the ImGui context and API storage for every DLL cycle.
    unsafe {
        imgui::igSetAllocatorFunctions(Some(allocate), Some(deallocate), ptr::null_mut());
        let context = imgui::igCreateContext(ptr::null_mut());
        let io = &mut *imgui::igGetIO();
        io.IniFilename = ptr::null();
        io.DisplaySize = imgui::ImVec2 {
            x: 1280.0,
            y: 720.0,
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

        for cycle in 0..10 {
            EVENTS.lock().unwrap().clear();
            assert!(RENDER_CALLBACKS.lock().unwrap().is_empty());
            assert!(INPUT_CALLBACK.lock().unwrap().is_none());
            let library = Library::open(Path::new(&dll));
            let entry = library.symbol(c"GetAddonDef");
            assert!(!entry.is_null(), "GetAddonDef export is missing");
            // SAFETY: Nexus defines GetAddonDef as this function signature.
            let get_definition = std::mem::transmute::<
                *mut c_void,
                unsafe extern "system" fn() -> *const AddonDefinition,
            >(entry);
            let definition = &*get_definition();
            assert_eq!(definition.api_version, AddonApi::VERSION);
            assert_eq!(CStr::from_ptr(definition.name), c"MannyUploader");
            assert_eq!(CStr::from_ptr(definition.author), c"Logan");
            assert_eq!(definition.provider, UpdateProvider::GitHub);
            assert_eq!(
                CStr::from_ptr(definition.update_link),
                c"https://github.com/LoganWal/GW2-MannyUploader"
            );
            let state = state_probe(&library, c"MannyLifecycleTestState");
            let jobs = state_probe(&library, c"MannyLifecycleTestJobs");
            assert_eq!(state.is_some(), active, "unexpected lifecycle-test exports");
            assert_eq!(jobs.is_some(), active, "unexpected lifecycle-test exports");
            let before = state.map_or(0, |probe| probe());

            (definition.load)(api);
            assert_eq!(
                &EVENTS.lock().unwrap()[..4],
                ["main", "options", "input", "shortcut"]
            );
            let callbacks = RENDER_CALLBACKS.lock().unwrap().clone();
            assert_eq!(callbacks.len(), 2);
            imgui::igNewFrame();
            for callback in callbacks {
                callback();
            }
            imgui::igRender();
            INPUT_CALLBACK.lock().unwrap().unwrap()(
                c"KB_MANNY_UPLOADER_TOGGLE_WINDOW".as_ptr(),
                true,
            );

            let (reached_active, active_status, active_jobs) = if active {
                let staged = stage_archives(&logs, cycle, jobs.unwrap());
                let (reached, status, job_status) = wait_for_active(state.unwrap(), jobs.unwrap());
                (staged && reached, status, job_status)
            } else {
                (true, 0, 0)
            };
            let unload_started = Instant::now();
            definition.unload.expect("addon must support hot unload")();
            let unload_elapsed = unload_started.elapsed();
            assert!(
                reached_active,
                "cycle {cycle} did not reach active uploads: state={active_status:#x}, jobs={active_jobs:#x}"
            );
            assert!(
                unload_elapsed < Duration::from_secs(5),
                "addon unload timed out"
            );
            if let (Some(state), Some(jobs)) = (state, jobs) {
                let after = state();
                assert_eq!(after >> 63, 0, "test transport reported a failure");
                assert_eq!(jobs(), 0, "jobs remained after unload");
                assert_eq!(
                    (after >> 48) & 0x7fff,
                    0,
                    "HTTP client remained after unload"
                );
                assert_eq!(
                    (after >> 32) & 0xffff,
                    0,
                    "active HTTP remained after unload"
                );
                assert_eq!(after & 0xffff, (before & 0xffff) + 2);
                assert_eq!((after >> 16) & 0xffff, ((before >> 16) & 0xffff) + 2);
            }
            assert!(RENDER_CALLBACKS.lock().unwrap().is_empty());
            assert!(INPUT_CALLBACK.lock().unwrap().is_none());
            assert!(EVENTS.lock().unwrap().ends_with(&[
                "remove shortcut",
                "remove input",
                "remove options",
                "remove main",
            ]));
            drop(library);
            let wide: Vec<u16> = Path::new(&dll)
                .as_os_str()
                .encode_wide()
                .chain([0])
                .collect();
            assert!(
                GetModuleHandleW(wide.as_ptr()).is_null(),
                "FreeLibrary retained the addon DLL"
            );
        }
        imgui::igSetAllocatorFunctions(Some(allocate), Some(deallocate), ptr::null_mut());
        imgui::igDestroyContext(context);
    }
    *TEST_PATHS.lock().unwrap() = None;
    std::fs::remove_dir_all(root).unwrap();
}
