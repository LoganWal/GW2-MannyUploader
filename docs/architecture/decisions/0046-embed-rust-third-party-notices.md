# ADR 0046: Embed Rust third-party notices

- Status: Accepted
- Date: 2026-10-17

## Decision

Update THIRD_PARTY_NOTICES.md against the 58 third-party packages in Cargo's locked Windows MSVC
dependency graph, including build dependencies. Retain the pinned source copyright notices and
license choices, the instant BSD notice, unicode-ident's Unicode License V3, and the bundled ImGui
font notice. The existing C++ notices remain present while that implementation is supported.

Compile the complete UTF-8 document into the Rust DLL as the language-neutral Windows RCDATA
resource MANNY_THIRD_PARTY_NOTICES. Cargo's native CMake project builds that resource alongside
libcurl, and the build script passes its generated artifact only to the cdylib linker. Source and
resource-template changes invalidate the Cargo build. No runtime filesystem read, additional DLL
export, or Nexus metadata change is required. The install ZIP still contains only the addon DLL.

Rust package configuration and packaged-copy checks require the complete current notice document
to occur byte-for-byte inside the DLL. Verification bounds file sizes, rejects empty notices, and
requires byte alignment when matching hexadecimal data. Portable CMake fixtures cover missing,
truncated, stale, empty, and unaligned matches. The prior Rust Release DLL failed this new gate.
Native resource inspection additionally confirms that the named RCDATA contains the exact source
document, including Unicode names and copyright text.

This provides notices with the DLL-only distribution. It does not publish a release or change the
remaining native in-game and shipping-cutover gates. Future dependency updates must also
review their notices before producing a new package.
