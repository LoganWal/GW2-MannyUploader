use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, SystemTime};

use manny_uploader::application::*;
use manny_uploader::domain::*;
use manny_uploader::filesystem::PollingLogSource;

fn file(name: &str, size: u64) -> LogFileIdentity {
    LogFileIdentity {
        canonical_path: name.into(),
        size,
        last_write_time: SystemTime::UNIX_EPOCH,
    }
}

#[test]
fn stability_tracks_interleaved_files_and_resets_for_size_or_time_changes() {
    let mut discovery = LogDiscovery::new(2, 2, 3).unwrap();
    let a = file("a.ZEVTC", 1);
    let b = file("b.zevtc", 2);
    assert_eq!(discovery.observe(a.clone()), Ok(None));
    assert_eq!(discovery.observe(b.clone()), Ok(None));
    let mut a_changed = a.clone();
    a_changed.last_write_time += Duration::from_secs(1);
    assert_eq!(discovery.observe(a_changed.clone()), Ok(None));
    assert_eq!(discovery.observe(b.clone()), Ok(Some(b.clone())));
    a_changed.size += 1;
    assert_eq!(discovery.observe(a_changed.clone()), Ok(None));
    assert_eq!(
        discovery.observe(a_changed.clone()),
        Ok(Some(a_changed.clone()))
    );
    assert_eq!(discovery.pending_count(), 0);
    for _ in 0..4 {
        assert_eq!(discovery.observe(a_changed.clone()), Ok(None));
        assert_eq!(discovery.observe(b.clone()), Ok(None));
    }
    assert_eq!(discovery.observe(a.clone()), Ok(None));
    assert_eq!(discovery.observe(a.clone()), Ok(Some(a)));
}

#[test]
fn validation_bounds_and_reconfiguration_preserve_accepted_identities() {
    assert!(matches!(
        LogDiscovery::new(1, 1, 1),
        Err(DiscoveryError::InvalidConfiguration)
    ));
    let mut discovery = LogDiscovery::new(2, 1, 2).unwrap();
    let a = file("a.zevtc", 1);
    let b = file("b.zevtc", 1);
    assert_eq!(
        discovery.observe(file("bad.evtc", 1)),
        Err(DiscoveryError::InvalidCandidate)
    );
    discovery.observe(a.clone()).unwrap();
    assert_eq!(
        discovery.observe(b.clone()),
        Err(DiscoveryError::ResourceLimit)
    );
    assert_eq!(
        discovery.set_required_matches(1),
        Err(DiscoveryError::InvalidConfiguration)
    );
    assert_eq!(discovery.observe(a.clone()), Ok(Some(a.clone())));
    discovery.observe(b.clone()).unwrap();
    discovery.set_required_matches(3).unwrap();
    assert_eq!(discovery.pending_count(), 0);
    assert_eq!(discovery.observe(a), Ok(None));
    assert_eq!(discovery.observe(b.clone()), Ok(None));
    discovery.forget_pending(&b.canonical_path);
    assert_eq!(discovery.observe(b.clone()), Ok(None));
    assert_eq!(discovery.observe(b.clone()), Ok(None));
    assert_eq!(discovery.observe(b.clone()), Ok(Some(b)));
}

#[test]
fn seeding_is_validated_before_mutation_and_fifo_does_not_refresh_duplicates() {
    let mut discovery = LogDiscovery::new(2, 2, 2).unwrap();
    let a = file("a.zevtc", 1);
    let b = file("b.zevtc", 1);
    let c = file("c.zevtc", 1);
    assert_eq!(
        discovery.seed(&[a.clone(), file("bad.txt", 1)]),
        Err(DiscoveryError::InvalidCandidate)
    );
    assert_eq!(discovery.accepted_count(), 0);
    discovery.seed(&[a.clone(), b.clone()]).unwrap();
    discovery.seed(std::slice::from_ref(&a)).unwrap();
    discovery.seed(std::slice::from_ref(&c)).unwrap();
    assert_eq!(discovery.observe(a.clone()), Ok(None));
    assert_eq!(discovery.observe(a.clone()), Ok(Some(a)));
    discovery.release(&c);
    assert_eq!(discovery.observe(c.clone()), Ok(None));
    assert_eq!(discovery.observe(c.clone()), Ok(Some(c)));
}

struct Tree(PathBuf);
impl Tree {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let root = std::env::temp_dir().join(format!(
            "manny-discovery-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&root).unwrap();
        Self(root)
    }
    fn write(&self, name: &str) -> PathBuf {
        let path = self.0.join(name);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, b"log").unwrap();
        fs::canonicalize(path).unwrap()
    }
    fn options(&self) -> CandidateSourceOptions {
        CandidateSourceOptions {
            root: self.0.clone(),
            recursive: true,
            max_candidates: 8,
            minimum_last_write_time: None,
        }
    }
}
impl Drop for Tree {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn polling_handles_unicode_sorted_results_removals_and_missing_root_recovery() {
    let tree = Tree::new();
    let stop = CancellationSource::default().token();
    let mut options = tree.options();
    options.root = tree.0.join("logs");
    let mut source = PollingLogSource::new(options).unwrap();
    assert!(!source.poll(&stop).unwrap().root_available);
    let b = tree.write("logs/nested/\u{96ea}.ZEVTC");
    let a = tree.write("logs/a.zevtc");
    tree.write("logs/ignored.evtc");
    fs::create_dir(tree.0.join("logs/directory.zevtc")).unwrap();
    let batch = source.poll(&stop).unwrap();
    assert!(batch.root_available && batch.scan_complete && batch.issues.is_empty());
    assert_eq!(
        batch
            .observations
            .iter()
            .map(|f| &f.canonical_path)
            .collect::<Vec<_>>(),
        vec![&a, &b]
    );
    assert_eq!(batch.observations[0].size, 3);
    assert_eq!(source.retained_count(), 2);
    fs::remove_file(&a).unwrap();
    assert_eq!(source.poll(&stop).unwrap().removed_paths, vec![a]);
    fs::remove_dir_all(tree.0.join("logs")).unwrap();
    let missing = source.poll(&stop).unwrap();
    assert!(!missing.root_available);
    assert_eq!(missing.removed_paths, vec![b]);
    let later = tree.write("logs/later.zevtc");
    assert_eq!(
        source.poll(&stop).unwrap().observations[0].canonical_path,
        later
    );
}

#[test]
fn limits_and_cancel_preserve_snapshot_and_cutoff_precedes_limit() {
    let tree = Tree::new();
    let a = tree.write("a.zevtc");
    let mut options = tree.options();
    options.max_candidates = 1;
    let mut source = PollingLogSource::new(options.clone()).unwrap();
    let stop = CancellationSource::default();
    source.poll(&stop.token()).unwrap();
    let b = tree.write("b.zevtc");
    assert_eq!(
        source.poll(&stop.token()),
        Err(CandidateError::ResourceLimit)
    );
    assert_eq!(source.retained_count(), 1);
    fs::remove_file(b).unwrap();
    stop.request_stop();
    assert_eq!(source.poll(&stop.token()), Err(CandidateError::Cancelled));
    assert_eq!(source.retained_count(), 1);
    fs::remove_file(a).unwrap();
    assert_eq!(
        source
            .poll(&CancellationSource::default().token())
            .unwrap()
            .removed_paths
            .len(),
        1
    );
    tree.write("old1.zevtc");
    tree.write("old2.zevtc");
    options.minimum_last_write_time = Some(SystemTime::now() + Duration::from_secs(60));
    source.reconfigure(options).unwrap();
    assert!(
        source
            .poll(&CancellationSource::default().token())
            .unwrap()
            .observations
            .is_empty()
    );
}

#[test]
fn reconfiguration_is_atomic_and_nonrecursive_scans_ignore_subdirectories() {
    let tree = Tree::new();
    tree.write("a.zevtc");
    tree.write("nested/b.zevtc");
    let stop = CancellationSource::default().token();
    let mut source = PollingLogSource::new(tree.options()).unwrap();
    assert_eq!(source.poll(&stop).unwrap().observations.len(), 2);
    let mut options = tree.options();
    options.root.clear();
    assert_eq!(
        source.reconfigure(options),
        Err(CandidateError::InvalidConfiguration)
    );
    assert_eq!(source.options(), &tree.options());
    assert_eq!(source.retained_count(), 2);
    let mut options = tree.options();
    options.recursive = false;
    source.reconfigure(options).unwrap();
    assert_eq!(source.retained_count(), 0);
    assert_eq!(source.poll(&stop).unwrap().observations.len(), 1);
    let mut options = tree.options();
    options.root = tree.0.join("a.zevtc");
    source.reconfigure(options).unwrap();
    assert_eq!(source.poll(&stop), Err(CandidateError::ScanFailed));
}

#[test]
fn cutoff_includes_exact_timestamp_and_newer_files() {
    let tree = Tree::new();
    let old = tree.write("old.zevtc");
    let current = tree.write("current.zevtc");
    let time = SystemTime::UNIX_EPOCH + Duration::from_secs(1_700_000_000);
    fs::File::options()
        .write(true)
        .open(&old)
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(time - Duration::from_secs(1)))
        .unwrap();
    fs::File::options()
        .write(true)
        .open(&current)
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(time))
        .unwrap();
    let mut options = tree.options();
    options.max_candidates = 1;
    options.minimum_last_write_time = Some(time);
    let mut source = PollingLogSource::new(options).unwrap();
    let batch = source.poll(&CancellationSource::default().token()).unwrap();
    assert_eq!(batch.observations.len(), 1);
    assert_eq!(batch.observations[0].canonical_path, current);
}

#[cfg(windows)]
#[test]
fn legacy_windows_history_suppresses_verbatim_canonical_path_observations() {
    use std::collections::HashSet;
    for (legacy, current) in [
        (
            "C:\\logs\\\u{96ea}.zevtc",
            "\\\\?\\C:\\logs\\\u{96ea}.zevtc",
        ),
        (
            "\\\\server\\share\\log.zevtc",
            "\\\\?\\UNC\\server\\share\\log.zevtc",
        ),
    ] {
        let stored = file(legacy, 123);
        let observed = file(current, 123);
        let mut discovery = LogDiscovery::new(2, 8, 8).unwrap();
        discovery.seed(std::slice::from_ref(&stored)).unwrap();
        assert_eq!(discovery.observe(observed.clone()), Ok(None));
        assert_eq!(discovery.observe(observed.clone()), Ok(None));
        let identities = HashSet::from([stored.clone(), observed.clone()]);
        assert_eq!(identities.len(), 1);
        assert_eq!(stored, observed);
        assert_ne!(stored, file(current, 124));
        let mut later = observed.clone();
        later.last_write_time += Duration::from_nanos(100);
        assert_ne!(stored, later);
    }
}

#[cfg(windows)]
#[test]
fn windows_junctions_are_neither_scanned_nor_accepted_as_roots() {
    use std::os::windows::fs::MetadataExt;
    use std::os::windows::process::CommandExt;
    use std::process::{Command, Stdio};
    let tree = Tree::new();
    let outside = Tree::new();
    outside.write("outside.zevtc");
    let junction = tree.0.join("junction");
    // Junction creation does not require Windows developer mode or symlink privileges.
    assert!(
        Command::new("cmd.exe")
            .creation_flags(0x0800_0000)
            .args(["/d", "/c", "mklink", "/J"])
            .arg(&junction)
            .arg(&outside.0)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .unwrap()
            .success()
    );
    // Some command interpreters return success without implementing junction creation.
    let metadata = fs::symlink_metadata(&junction).expect("junction fixture was not created");
    assert_ne!(
        metadata.file_attributes() & 0x10,
        0,
        "junction fixture must have the directory attribute"
    );
    assert_ne!(
        metadata.file_attributes() & 0x400,
        0,
        "junction fixture must be a reparse point"
    );
    assert!(
        junction.join("outside.zevtc").is_file(),
        "junction fixture must reach its target"
    );
    let mut source = PollingLogSource::new(tree.options()).unwrap();
    let stop = CancellationSource::default().token();
    assert!(source.poll(&stop).unwrap().observations.is_empty());
    let mut options = tree.options();
    options.root = junction.clone();
    source.reconfigure(options).unwrap();
    assert_eq!(source.poll(&stop), Err(CandidateError::ScanFailed));
    fs::remove_dir(junction).unwrap();
    assert!(outside.0.join("outside.zevtc").is_file());
}

#[cfg(unix)]
#[test]
fn symlink_files_directories_and_roots_are_not_followed() {
    use std::os::unix::fs::symlink;
    let tree = Tree::new();
    let outside = Tree::new();
    let file = outside.write("outside.zevtc");
    symlink(&file, tree.0.join("linked.zevtc")).unwrap();
    symlink(&outside.0, tree.0.join("directory")).unwrap();
    let stop = CancellationSource::default().token();
    let mut source = PollingLogSource::new(tree.options()).unwrap();
    assert!(source.poll(&stop).unwrap().observations.is_empty());
    let mut options = tree.options();
    options.root = tree.0.join("directory");
    source.reconfigure(options).unwrap();
    assert_eq!(source.poll(&stop), Err(CandidateError::ScanFailed));
}
