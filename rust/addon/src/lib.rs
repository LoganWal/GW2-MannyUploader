//! Rust development addon with native composition and snapshot-driven Nexus rendering.

#[cfg(all(windows, not(target_arch = "x86_64")))]
compile_error!("Nexus requires Windows x64");
#[cfg(panic = "abort")]
compile_error!("The addon requires unwinding to contain callback panics");

#[cfg(windows)]
pub mod composition;
#[cfg(windows)]
pub mod directory_monitor;
#[cfg(windows)]
pub mod dpapi;
#[cfg(windows)]
pub mod external_actions;
#[cfg(windows)]
pub mod file_commit;
#[cfg(windows)]
mod host;
#[cfg(windows)]
pub mod http;
#[cfg(windows)]
mod icons;
#[cfg(all(windows, feature = "lifecycle-test"))]
mod lifecycle_test;
#[cfg(windows)]
pub mod random;
#[cfg(windows)]
mod runtime;
#[cfg(windows)]
mod ui;

#[cfg(all(test, windows))]
pub(crate) static IMGUI_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
