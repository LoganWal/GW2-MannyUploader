# ADR 0004: Use native user-scoped DPAPI and fail closed under Wine

- Status: Superseded by ADR 0007
- Date: 2026-08-20

## Context

dps.report, DonBot, and Twitch need credentials that must survive addon restarts without entering the
ordinary JSON settings file. The addon is a Windows x64 DLL, but Guild Wars 2 and Nexus may run either
on native Windows or through Wine. A storage choice must not claim Windows security properties where
the compatibility implementation does not provide them.

Options considered:

- **Windows DPAPI (`CryptProtectData`):** built into Windows, user-scoped by default, includes keyed
  integrity, requires no bundled key or companion DLL, and has a prompt-free mode.
- **Windows Credential Manager:** convenient record lifecycle, but does not improve Wine security;
  Wine's generic credential implementation encrypts registry data with a key stored beside it.
- **Wine DPAPI:** API-compatible enough for round trips, but the current Wine source documents that it
  does not know the Windows keying mechanism, ignores protection flags, and derives its key using the
  username and a public static implementation secret.
- **Host Secret Service/KWallet bridge:** potentially correct for Wine, but requires a separately
  packaged and tested native bridge or a substantial D-Bus implementation. It is not a small DLL-only
  adapter and is not proven for the first implementation slice.
- **Plaintext, reversible obfuscation, or a key embedded in the addon:** rejected because they provide
  no meaningful protection from another process or copied configuration directory.

Native DPAPI behavior is documented by Microsoft in
[`CryptProtectData`](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)
and
[`CryptUnprotectData`](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptunprotectdata).
The compatibility assessment is based on Wine's generated
[`CryptProtectData` API page](https://source.winehq.org/WineAPI/CryptProtectData.html), current
[`protectdata.c`](https://github.com/wine-mirror/wine/blob/master/dlls/crypt32/protectdata.c), and
current [`cred.c`](https://github.com/wine-mirror/wine/blob/master/dlls/advapi32/cred.c).

## Decision

Use a file-backed protected store whose cryptographic adapter is native, user-scoped DPAPI. Pass
`CRYPTPROTECT_UI_FORBIDDEN`, omit machine scope, bind fixed application entropy, wipe returned
plaintext allocations with `SecureZeroMemory`, and add a versioned bounded envelope with an ID and
CRC-32 corruption check.

Before constructing DPAPI storage, detect the Wine-owned `ntdll!wine_get_version` export. If present,
return `UnsupportedEnvironment`. Do not call Wine DPAPI and do not fall back to Wine Credential
Manager, plaintext, or embedded-key encryption.

Store the Twitch OAuth session as one record so refresh-token rotation is one atomic protected-file
replacement. Do not keep secret backups. Ordinary settings and UI snapshots contain connection state
only, never credentials.

## Consequences

- Native Windows gets durable credentials without another runtime dependency.
- Copying a protected record to another normal local user or computer does not intentionally make it
  decryptable; roaming-profile exceptions remain Windows policy.
- Wine users cannot persist provider credentials in this version. The UI must report secure storage
  as unavailable rather than offering a misleading connect/save workflow.
- A future Wine solution needs its own ADR and threat review, most likely around a host keyring bridge.
- Native Windows CI must run real protect/unprotect/tamper tests. Wine validation must prove the
  adapter fails before writing.
- Best-effort memory wiping narrows exposure but does not make general process memory a secure enclave.
