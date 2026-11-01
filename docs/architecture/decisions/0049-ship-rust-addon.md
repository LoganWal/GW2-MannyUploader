# ADR 0049: Ship the Rust addon

- Status: Accepted
- Date: 2026-11-01

## Decision

The Rust DLL is the sole shipping implementation. The manual Windows x64 release job builds the
MSVC Rust DLL, validates native DPAPI and transport behavior, verifies its image and matching PDB,
packages it with CPack, and runs ten-cycle smoke checks against the packaged copy. The automatic
Build DLL job continues to provide a MinGW development artifact.

Remove the C++ addon implementation, its unit tests, and its old CPack path. A Rust real-DLL smoke
test replaces the temporary C++ Nexus smoke host and checks ten load, render, unload, and
`FreeLibrary` cycles against Debug, Release, and packaged DLLs. Active-provider unload uses a
test-only Rust DLL and never reaches the shipping package. CMake remains for native curl and Rust
packaging. Historical ADRs and provider contracts remain as design records. This supersedes the
C++ shipping choice and smoke-host choice in ADR 0044.

## Release gate

The hosted Windows MSVC job remains the authoritative native gate. Local MinGW builds and smoke
tests do not establish Microsoft STL or in-game behavior. Complete the native in-game release matrix
before publishing a release. Do not restore Wine validation as a prerequisite; Wine runtime support
and its honest reduced-protection warning remain required under ADR 0048.
