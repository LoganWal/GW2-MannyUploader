# GW2 Manny Uploader release evidence

Copy this document for each release candidate. Store only hashes, identifiers, safe diagnostics, and
links. Never record API keys, OAuth tokens, Device Codes, protected files, raw provider responses, or
unredacted account/path information.

## Candidate identity

- Version:
- Commit SHA:
- Candidate date (UTC):
- Maintainer:
- GitHub workflow URL:
- Windows runner image and version:
- Nexus version:
- Guild Wars 2 build:
- arcdps build:

## External release gates

- [ ] Production public Twitch application ID was injected into the build.
- [ ] Nexus addon signature is reserved or confirmed:
- [ ] Listing decision is recorded as new addon or intentional successor:
- [ ] Addon metadata version matches the candidate version.

## Automated native gate

- [ ] Windows x64 Release configuration and build passed with warnings treated as errors.
- [ ] Native DPAPI round-trip, plaintext separation, and tamper rejection passed.
- [ ] Deterministic HTTP loopback and real directory-notification contracts passed.
- [ ] CPack produced exactly one root-DLL ZIP and matching checksum.
- [ ] The packaged DLL passed ten Nexus hot-load/unload cycles.
- [ ] Exactly one linker PDB was staged, verified, and checksummed separately.
- [ ] Optional Schannel HTTPS probe passed, if required for this candidate.

## Artifacts

| Artifact | Filename | SHA-256 |
| --- | --- | --- |
| Install ZIP |  |  |
| Linker PDB |  |  |

- Install ZIP contents:
- Symbols artifact contents:
- Artifact retention or release URL:

## Native in-game matrix

Complete every row in
[`native-windows-validation.md`](native-windows-validation.md) and record only safe results here.

- Result: Pass / Fail
- Tester:
- Test installation:
- Failed or waived rows:
- Safe notes:

## Approval

- [ ] No release blocker remains open.
- [ ] Installation and configuration documentation matches the candidate.
- [ ] Upgrade behavior from the previous public version is documented.
- [ ] Package, checksum, PDB, checksum, workflow, and this evidence record refer to the same commit.
- Release decision: Approved / Rejected
- Approved by:
- Approval date (UTC):
