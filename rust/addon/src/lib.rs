//! Development lifecycle shell. Uploading is not implemented in this DLL yet.

#[cfg(all(windows, not(target_arch = "x86_64")))]
compile_error!("Nexus requires Windows x64");
#[cfg(panic = "abort")]
compile_error!("The addon requires unwinding to contain callback panics");

#[cfg(windows)]
mod host;
#[cfg(windows)]
mod icons;
#[cfg(windows)]
mod runtime;
