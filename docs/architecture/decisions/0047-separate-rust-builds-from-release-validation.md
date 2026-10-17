# ADR 0047: Separate Rust builds from release validation

- Status: Accepted
- Date: 2026-10-17

## Decision

Use a single Ubuntu/MinGW `Build DLL` workflow for ordinary pushes and pull requests, following
the maintainer's Carrot Quickness build pattern. It checks Rust formatting, lints the Windows GNU
target, runs portable core and validation-tool tests, and builds the Rust Release DLL with Cargo.
Cache Cargo dependencies and build output. Strip embedded GNU debug information and upload the DLL
as a commit-named development artifact. The manual MSVC workflow retains separate linker PDBs.

The existing libcurl/Schannel backend still builds through Cargo's build script. A small CMake
toolchain selects the Linux MinGW compiler and resource compiler. Rust's Windows target uses the
same POSIX MinGW toolchain and static C++ runtime. Check the final DLL for companion GNU runtime
imports and verify its embedded third-party notices before uploading it.

Move the previous CI workflow to `Release validation` with manual dispatch only. Preserve its
MSVC, C++, Wine, lifecycle, packaging, and symbol checks. This changes when those checks run,
without treating a successful cross-build as native runtime evidence or clearing outstanding
Wine failures. ADRs 0044 and 0045 continue to define the full validation artifacts and checks.

Keep the registered `.github/workflows/ci.yml` path so manual runs can select `rust-migration`
before this branch is merged. GitHub requires a dispatchable workflow to exist on the default branch.
See [manual workflow runs](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).

The ordinary action does not publish releases or install the DLL. The C++ shipping artifact and
Rust development metadata remain unchanged until the migration's release gates are satisfied.
