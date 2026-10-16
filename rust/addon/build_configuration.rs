use std::{ffi::OsString, process::Command};

pub(crate) fn configure_generator(
    command: &mut Command,
    generator: Option<OsString>,
    native_msvc: bool,
) {
    if let Some(generator) = generator {
        command.arg("-G").arg(generator);
    } else if native_msvc {
        // CMake selects the installed Visual Studio version, including newer hosted images.
        command.args(["-A", "x64"]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_msvc_accepts_the_installed_visual_studio() {
        for installed in ["Visual Studio 17 2022", "Visual Studio 18 2026"] {
            let mut command = Command::new("cmake");
            configure_generator(&mut command, None, true);
            let arguments: Vec<_> = command.get_args().collect();
            let selected = arguments
                .windows(2)
                .find(|pair| pair[0] == "-G")
                .map(|pair| pair[1].to_str().unwrap())
                .unwrap_or(installed);
            assert_eq!(
                selected, installed,
                "CMake cannot use an uninstalled generator"
            );
            assert!(arguments.windows(2).any(|pair| pair == ["-A", "x64"]));
        }
    }

    #[test]
    fn explicit_and_cross_generators_keep_their_own_platform_selection() {
        for native_msvc in [false, true] {
            let mut command = Command::new("cmake");
            configure_generator(&mut command, Some("Ninja".into()), native_msvc);
            let arguments: Vec<_> = command.get_args().collect();
            assert!(arguments.windows(2).any(|pair| pair == ["-G", "Ninja"]));
            assert!(!arguments.contains(&std::ffi::OsStr::new("-A")));
        }
        let mut cross = Command::new("cmake");
        configure_generator(&mut cross, None, false);
        assert_eq!(cross.get_args().count(), 0);
    }
}
