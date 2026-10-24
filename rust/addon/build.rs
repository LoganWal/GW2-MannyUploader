use std::env;
use std::path::PathBuf;
use std::process::Command;

mod build_configuration;
use build_configuration::configure_generator;

fn run(command: &mut Command) {
    assert!(
        command.status().expect("could not start CMake").success(),
        "native HTTP build failed"
    );
}

fn main() {
    println!("cargo:rerun-if-env-changed=MANNY_ADDON_VERSION");
    let version = env::var("MANNY_ADDON_VERSION").unwrap_or_else(|_| "2026.24.10.46".into());
    let components: Vec<i16> = version
        .split('.')
        .map(|part| part.parse().expect("addon version components must fit i16"))
        .collect();
    assert_eq!(
        components.len(),
        4,
        "addon version must be year.day.month.build"
    );
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    std::fs::write(
        out.join("addon_version.rs"),
        format!(
            "AddonVersion {{ major: {}, minor: {}, build: {}, revision: {} }}",
            components[0], components[1], components[2], components[3]
        ),
    )
    .expect("could not write addon version");
    println!("cargo:rerun-if-env-changed=MANNY_TWITCH_CLIENT_ID");
    let client_id = env::var("MANNY_TWITCH_CLIENT_ID").unwrap_or_default();
    let client_id = if client_id.is_empty() {
        "s1q7vb9ktz0efvs61axfngdegolo6q"
    } else {
        &client_id
    };
    assert!(
        client_id.len() <= 128
            && client_id
                .bytes()
                .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit()),
        "MANNY_TWITCH_CLIENT_ID must contain at most 128 lowercase ASCII letters and digits"
    );
    println!("cargo:rustc-env=MANNY_PACKAGED_TWITCH_CLIENT_ID={client_id}");
    for file in [
        "build.rs",
        "build_configuration.rs",
        "native/CMakeLists.txt",
        "native/notices.rc.in",
        "../../cmake/MannyCurl.cmake",
        "../../THIRD_PARTY_NOTICES.md",
    ] {
        println!("cargo:rerun-if-changed={file}");
    }
    for name in [
        "MANNY_RUST_CMAKE",
        "MANNY_RUST_CMAKE_GENERATOR",
        "MANNY_RUST_CMAKE_TOOLCHAIN",
        "MANNY_CURL_ARCHIVE",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    assert_eq!(
        env::var("CARGO_CFG_TARGET_ARCH").as_deref(),
        Ok("x86_64"),
        "Nexus requires Windows x64"
    );
    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let build = PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("n");
    let cmake = env::var_os("MANNY_RUST_CMAKE").unwrap_or_else(|| "cmake".into());
    let profile = if env::var("PROFILE").as_deref() == Ok("release") {
        "Release"
    } else {
        "RelWithDebInfo"
    };
    let mut configure = Command::new(&cmake);
    configure
        .arg("-S")
        .arg(manifest.join("native"))
        .arg("-B")
        .arg(&build)
        .arg(format!("-DCMAKE_BUILD_TYPE={profile}"));
    if let Some(toolchain) = env::var_os("MANNY_RUST_CMAKE_TOOLCHAIN") {
        configure.arg("--toolchain").arg(toolchain);
    } else {
        assert!(
            env::var("HOST").unwrap().contains("windows"),
            "cross compilation requires MANNY_RUST_CMAKE_TOOLCHAIN"
        );
    }
    configure_generator(
        &mut configure,
        env::var_os("MANNY_RUST_CMAKE_GENERATOR"),
        env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") && cfg!(windows),
    );
    if let Some(archive) = env::var_os("MANNY_CURL_ARCHIVE") {
        let mut argument = std::ffi::OsString::from("-DMANNY_CURL_ARCHIVE=");
        argument.push(archive);
        configure.arg(argument);
    } else {
        configure.arg("-DMANNY_CURL_ARCHIVE=");
    }
    run(&mut configure);
    run(Command::new(&cmake)
        .arg("--build")
        .arg(&build)
        .args([
            "--config",
            profile,
            "--target",
            "libcurl_static",
            "manny_notices",
            "--parallel",
        ])
        .arg(env::var("NUM_JOBS").unwrap_or_else(|_| "4".into())));
    let library = std::fs::read_to_string(build.join(format!("curl-library-{profile}.txt")))
        .expect("native library manifest is missing");
    let library = PathBuf::from(library.trim());
    assert!(library.is_file(), "native library is missing");
    let directory = library.parent().unwrap();
    let name = library.file_stem().unwrap().to_str().unwrap();
    let name = if env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("gnu") {
        name.strip_prefix("lib").unwrap_or(name)
    } else {
        name
    };
    println!("cargo:rustc-link-search=native={}", directory.display());
    println!("cargo:rustc-link-lib=static={name}");
    let resource = std::fs::read_to_string(build.join(format!("notices-resource-{profile}.txt")))
        .expect("third-party notices resource manifest is missing");
    let resource = PathBuf::from(resource.trim());
    assert!(
        resource.is_file(),
        "third-party notices resource is missing"
    );
    println!("cargo:rustc-link-arg-cdylib={}", resource.display());
    for library in [
        "ws2_32", "crypt32", "secur32", "bcrypt", "advapi32", "normaliz", "iphlpapi",
    ] {
        println!("cargo:rustc-link-lib={library}");
    }
}
