# ADR 0044: Package Rust migration builds

- Status: Accepted
- Date: 2026-10-17

## Decision

Keep the C++ shipping package while producing a separately named Rust CI artifact for migration
validation. The Cargo Release profile retains full debug information and optimization. Static CRT
linking and panic unwinding remain unchanged.

The independent rust/package CMake project consumes an explicitly selected Cargo MSVC Release
directory. It uses the addon manifest version and stages one DLL for the existing CPack ZIP/checksum
format. The top-level Cargo PDB is selected explicitly because Cargo also stores it under deps.
Symbols are staged separately from the install tree and checked by the existing size, signature,
name, and SHA-256 rules.

Microsoft dumpbin verifies a Windows x64 DLL with exactly one named export, GetAddonDef, a CodeView
linker-PDB reference, and only known Windows or API-set imports. Unexpected runtime or companion DLL
dependencies fail validation. The lifecycle-test exports cannot pass this boundary. Portable CMake
contract tests cover malformed images, exports, symbol references, and dynamic runtime dependencies.

A PDB signature check alone accepts symbols from the wrong build. The package also uses dumpbin
[/PDBPATH](https://learn.microsoft.com/en-us/cpp/build/reference/pdbpath?view=msvc-170) to require
that the selected PDB corresponds to the DLL. The supplied symbol directory replaces inherited
symbol-server paths. A different file found through another debugger search path is rejected.
Source and staged DLL/PDB pairs are both checked. A native integration test first accepts a matching
pair, then replaces the PDB with another build's symbols and requires rejection.

The CI release job depends on the Rust test matrix as well as the existing gates. It creates and
extracts the Rust ZIP, verifies its checksum and single-file layout, runs both ten-cycle smoke modes
against that packaged copy, and uploads symbols separately. C++ artifacts retain their current names.

## Remaining gates

Local native package evidence does not establish hosted execution, Wine parity, browser/clipboard
behavior, or the in-game release matrix. The Rust DLL retains development metadata. This decision
does not switch the shipping implementation or publish a public release.
