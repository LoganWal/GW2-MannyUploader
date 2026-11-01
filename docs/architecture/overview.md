# Architecture overview

GW2 Manny Uploader ships as a Rust Windows x64 Nexus API v6 addon. The portable core in `rust/src`
owns job state, settings, delivery policy, history, and application coordination. `rust/addon`
owns the Nexus ABI, ImGui rendering, filesystem and HTTP adapters, protected credentials, and
worker lifetime. Native callbacks submit commands and read immutable snapshots. Network, parsing,
credential, and persistence work runs outside render callbacks.

The coordinator alone mutates upload jobs. Provider and parser workers exchange bounded value
requests and results with it. When dps.report is selected, Wingman, DonBot, and Twitch wait for the
same job's valid permalink. Without dps.report, Wingman and DonBot upload the archive directly.
Persistent identities and receipts prevent automatic replay across restart and selection changes.
Only explicit Reupload and Rechat actions replay settled work.

The addon owns a callback admission gate and joined workers. Unload closes admission, unregisters
Nexus resources in reverse order, drains callbacks, and joins the runtime. Native Windows and Wine
retain the same user-visible features. Wine may use different filesystem and protected-storage
machinery, with reduced protection labeled in options.

The automatic [Build DLL workflow](../../.github/workflows/build.yml) produces a MinGW development
artifact. The manual [Release validation workflow](../../.github/workflows/ci.yml) builds the Rust
MSVC DLL, checks native DPAPI and Schannel behavior, runs Rust real-DLL smoke tests,
and verifies the CPack ZIP and matching PDB. The shipping package contains only the Rust DLL.
In-game validation remains a separate release gate.
After those gates pass, the manual [Publish Nexus release workflow](../../.github/workflows/publish-release.yml)
publishes the verified standalone DLL and package from the matching validation run.

Behavior boundaries are recorded in [contracts](../contracts/README.md). The
[Rust shipping decision](decisions/0049-ship-rust-addon.md) and
[Nexus release decision](decisions/0050-publish-nexus-updates.md) record the current release design.
The [Rust addon guide](../../rust/README.md) describes the implementation.
