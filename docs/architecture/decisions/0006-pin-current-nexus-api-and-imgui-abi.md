# ADR 0006: Pin the current Nexus API and ImGui ABI

- Status: Accepted
- Date: 2026-08-22

## Context

Nexus passes an ImGui context and allocator functions across the DLL boundary. An addon must therefore
compile against a compatible ImGui layout as well as the matching Nexus C header. Selecting either
dependency independently risks memory corruption even when the exported Nexus function signatures
still compile.

Raidcore's current C++ addon template commit
`3a6bfe6644be518b1b0ebd54bf08034deeac6c71` pins Nexus API commit
`9b2c53df86c00db6495642bfcff2d0611bd957ef` (API version 6) and Raidcore's Dear ImGui commit
`58075c4414b985b352d10718b02a8c43f25efd7c` (ImGui 1.80). Raidcore also maintains a separate ImGui
1.92.7 fork for its announced Nexus architecture upgrade, but the current template has not moved to
that ABI.

## Decision

- Pin and hash-verify the exact Nexus API and ImGui commits used by the current official template.
- Fetch these Windows-only dependencies through CMake; the portable core and Linux tests do not
  include either external ABI.
- Compile the required ImGui translation units into the addon and install the allocator and context
  supplied by Nexus before any ImGui call.
- Keep Nexus types inside the native entry adapter. The lifecycle, runtime, application, and UI model
  boundaries use project-owned types and remain independently testable.
- Upgrade Nexus API and ImGui only as one reviewed compatibility change after the official template
  adopts the new host ABI.

## Consequences

- Current Nexus compatibility is reproducible and does not depend on moving branches or tags.
- The addon cannot accidentally mix the current Nexus host with the future ImGui 1.92.7 ABI.
- A future Nexus rewrite is localized to dependency pins and the native adapter rather than the
  application core.
