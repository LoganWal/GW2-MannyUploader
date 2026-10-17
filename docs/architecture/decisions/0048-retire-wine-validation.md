# ADR 0048: Retire Wine validation

- Status: Accepted
- Date: 2026-10-17

## Decision

At the maintainer's request, keep Wine runtime support and remove the Wine validation machinery
that was blocking migration. This supersedes ADR 0045 and the Wine-specific validation requirements
in earlier migration decisions. Historical Wine failures are not recorded as passing.

Remove the hosted Wine job, its artifact staging and uploads, the Wine runner and its tests, and
the local MSVC-under-Wine preflight. Remove mandatory Wine test and in-game validation gates.
The everyday Cargo/MinGW DLL build remains automatic. The manual Release validation workflow
retains Linux and native Windows tests, native MSVC lifecycle checks, DPAPI and Schannel coverage,
and package/symbol verification. Native Windows in-game checks remain required for release.

Keep Wine detection, directory polling, protected-storage compatibility mode and its honest
reduced-protection warning, Unicode handling, cancellation, and orderly unload. Existing tests
for these shared behaviors remain. Optional Wine runs may still use the DPAPI test requirement
flag, but no Wine environment is required for ordinary builds or release validation.

Wine users retain the same features. Reproducible runtime bugs remain bugs to investigate through
the normal issue workflow.
