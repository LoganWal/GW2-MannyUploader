use std::os::windows::ffi::OsStrExt;
use std::path::Path;

use manny_core::application::FileError;
use manny_core::filesystem::AtomicFileCommit;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn MoveFileExW(existing: *const u16, replacement: *const u16, flags: u32) -> i32;
}

pub struct WindowsFileCommit;
impl AtomicFileCommit for WindowsFileCommit {
    fn replace(&self, temporary: &Path, destination: &Path) -> Result<(), FileError> {
        let from = native_path(temporary)?;
        let to = native_path(destination)?;
        // SAFETY: Both buffers are live, NUL-terminated UTF-16 paths. Same-directory staging permits
        // replacement without COPY_ALLOWED. Flags request replacement and synchronous completion.
        if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 0x01 | 0x08) } == 0 {
            Err(FileError::ReplaceFailed)
        } else {
            Ok(())
        }
    }

    fn sync_parent(&self, _: &Path) -> Result<(), FileError> {
        // Windows replacement uses MOVEFILE_WRITE_THROUGH. Match the existing native delete policy.
        Ok(())
    }
}

fn native_path(path: &Path) -> Result<Vec<u16>, FileError> {
    let filename = path.file_name().ok_or(FileError::InvalidPath)?;
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    // Canonicalize only the parent, preserving replacement of the named entry rather than a link's
    // target. Windows canonical paths include extended-length prefixes for Unicode/long paths.
    let absolute = std::fs::canonicalize(parent)
        .map_err(|_| FileError::ReplaceFailed)?
        .join(filename);
    let mut wide: Vec<u16> = absolute.as_os_str().encode_wide().collect();
    if wide.contains(&0) {
        return Err(FileError::InvalidPath);
    }
    wide.push(0);
    Ok(wide)
}

#[cfg(test)]
mod tests {
    use super::*;
    use manny_core::application::FileStore;
    use manny_core::filesystem::{FilesystemStore, temporary_path};
    use std::fs;
    use std::os::windows::fs::OpenOptionsExt;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    #[test]
    fn native_atomic_replace_supports_long_unicode_paths_and_preserves_locked_primary() {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let root = std::env::temp_dir().join(format!(
            "manny-native-store-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&root).unwrap();
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
        let _cleanup = Cleanup(root.clone());
        let path = root
            .join("a".repeat(100))
            .join("b".repeat(100))
            .join("c".repeat(100))
            .join("\u{96ea}.json");
        let store = FilesystemStore::new(WindowsFileCommit);
        store.write_atomic(&path, b"first").unwrap();
        store.write_atomic(&path, b"second").unwrap();
        assert_eq!(store.read(&path, 64).unwrap().unwrap(), b"second");
        let locked = fs::OpenOptions::new()
            .read(true)
            .share_mode(0)
            .open(&path)
            .unwrap();
        assert_eq!(
            store.write_atomic(&path, b"failed"),
            Err(FileError::ReplaceFailed)
        );
        drop(locked);
        assert_eq!(store.read(&path, 64).unwrap().unwrap(), b"second");
        assert!(temporary_path(&path).is_file());
        store.write_atomic(&path, b"recovered").unwrap();
        assert_eq!(store.read(&path, 64).unwrap().unwrap(), b"recovered");
        store.erase(&path).unwrap();
        store.erase(&path).unwrap();
        assert_eq!(store.read(&path, 64), Ok(None));
    }
}
