# Native Windows release validation

- Status: Required before a public release
- Date: 2026-08-22
- Scope: MSVC, DPAPI, Schannel, packaged Nexus DLL, and in-game behavior

Copy [`evidence-template.md`](evidence-template.md) for each candidate and keep it with the matching
workflow, package, checksums, and safe manual-test results.

## Automated native gate

The `Windows x64 release package` CI job is the authoritative native build. It must pass on an
ordinary Microsoft-hosted Windows runner, not Wine, and must use the x64 Release configuration with
warnings treated as errors.

The job sets `MANNY_REQUIRE_NATIVE_DPAPI=1`, requiring a real user-scoped DPAPI store round trip,
ciphertext/plaintext separation, and tamper rejection. The same suite exercises the
production libcurl adapter through deterministic loopback requests, including streaming, limits,
redirect refusal, cancellation, and error redaction.

The Windows suite also exercises the production directory-change monitor against a real temporary
directory. It verifies initial arming, idle polling, create and append notifications, missing-root
recovery, live reconfiguration, and cancellation with a bounded wait for operating-system delivery.

The job then creates the versioned CPack ZIP and checksum, verifies and extracts the archive, and
runs the ten-cycle Nexus smoke host against the packaged DLL. A passing build-tree DLL is not a
substitute for this packaged-copy check. The optimized build also emits a full linker PDB outside the
install tree. CI rejects missing or ambiguous linker-PDB output, stages exactly one candidate at the
canonical artifact path, then verifies its Microsoft Program Database signature, plausible size,
fixed name, and SHA-256 sidecar before uploading it as a separate artifact.

An independent Ubuntu job checks every project-owned C and C++ source with the repository's pinned
format policy. Fetched dependencies and generated build files are outside that source set.

## Opt-in Schannel probe

Ordinary pushes and pull requests never depend on a public network service. To verify the host trust
store and production Schannel path, manually run the `CI` workflow from the repository's Actions tab
with `run_live_https_probe` enabled. The probe performs one credential-free GET to the fixed
`https://example.com/` target through the production HTTPS-only policy and requires status `200`.

The probe is evidence for the selected Windows runner only. Record the workflow URL and Windows image
version in the release notes; do not generalize one successful probe into a guarantee for every user
environment.

## Wine compatibility gate

Cross-build the complete Windows test suite and run it under the maintained Wine environment with
`MANNY_REQUIRE_WINE_DPAPI=1`. This requires Wine detection plus the same protected-store round trip,
ciphertext/plaintext separation, and tamper rejection used by the native gate. The remaining suite
exercises the shared Windows HTTP, archive, configuration, provider, and application behavior; a
failure is a Wine release blocker.

The in-game matrix below must also be repeated under Wine for log-directory access, Nexus rendering,
all four enabled destinations, credential restart recovery, browser launch, and unload/reload. Wine
uses polling automatically if its implementation of the native directory notification API fails;
that fallback is supported behavior rather than a disabled feature.

## Packaged-artifact checks

Before installing the candidate in Guild Wars 2:

- [ ] Download the verified CPack ZIP and its `.sha256` sidecar from the same workflow run.
- [ ] Confirm the sidecar matches the downloaded ZIP.
- [ ] Confirm the ZIP contains only `manny_uploader.dll` at its root.
- [ ] Confirm the separately uploaded symbols artifact contains only `manny_uploader.pdb` and its
      matching `.sha256` sidecar; do not install either file into the Nexus addons directory.
- [ ] Confirm the build used the registered public Twitch client ID when Twitch is in release scope.
- [ ] Keep the exact workflow URL, commit, version, ZIP, package checksum, PDB checksum, and test
      output together.

## In-game Nexus matrix

Run this matrix on a supported native Windows installation with the release candidate installed:

- [ ] Nexus loads the addon without a companion runtime DLL or loader warning.
- [ ] The main window opens from quick access and the configured keybind, and both survive rebinding.
- [ ] Main and options windows render, close, and reopen without blocking the game thread.
- [ ] A missing log directory reports a waiting state and is discovered after creation.
- [ ] Changing directory, recursion, polling, stability, parser capacity, and history options applies
      live without duplicating an accepted log or cancelling active work.
- [ ] With dps.report enabled, a real `.zevtc` uploads there once and its permalink imports into
      GW2Wingman and DonBot.
- [ ] With dps.report disabled, a real `.zevtc` uses the direct GW2Wingman and DonBot upload paths.
- [ ] Enabling direct Wingman and DonBot together produces no duplicate Wingman submission from
      DonBot (`wingman=false` remains in effect).
- [ ] DonBot key verification, authorized-guild selection, restart recovery, disable, and disconnect
      behave as shown in options.
- [ ] Twitch Device Code connection targets only the authenticated broadcaster's own chat.
- [ ] Twitch test delivery, successful-encounter posting, and failed-encounter posting obey their
      independent options and contain the expected dps.report permalink.
- [ ] Restart preserves native-Windows credentials while ordinary JSON contains no credentials.
- [ ] Disconnect erases the relevant protected credential and leaves unrelated providers usable.
- [ ] Offline, timeout, `429`, permanent rejection, and ambiguous Twitch delivery present actionable
      states without unsafe automatic duplicate posting.
- [ ] Unloading or exiting during parsing, upload, authentication, and retry joins work promptly; ten
      manual unload/reload cycles leave no registered callback, keybind, shortcut, or locked DLL.
- [ ] Unicode paths and account/encounter/message text render and upload correctly.

Do not record keys, tokens, Device Codes, protected files, or raw provider responses in the evidence.
Any failed row is a release blocker unless it is explicitly removed from the supported version-1
scope and the product documentation is updated first.

## External release gates

Automation cannot complete these decisions:

- register and inject the production public Twitch application ID;
- reserve or confirm the Nexus addon signature; and
- choose a new Nexus listing or an intentional successor identity.

Record those outcomes before calling the artifact a release candidate.
