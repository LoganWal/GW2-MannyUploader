# ADR 0007: Enable Wine DPAPI as an explicit compatibility mode

- Status: Accepted
- Date: 2026-08-22
- Supersedes: ADR 0004's fail-closed Wine decision

## Context

DonBot and Twitch are required in the addon's real Wine/Proton deployment environment. ADR 0004
disabled both because Wine's `CryptProtectData` implementation does not provide Windows DPAPI's
user-scoped security. A host Secret Service bridge would provide a stronger boundary but would add a
native companion process or a substantial D-Bus implementation, conflicting with the single-DLL
release for this version.

Wine's current `protectdata.c` derives its symmetric key from the username, a public static
implementation string, per-record salt, and caller-supplied entropy. It provides encrypted storage,
round-trip behavior, and tamper detection, but an attacker with a copied profile can reproduce the
key inputs. The addon entropy is application binding, not a secret.

## Decision

Allow the existing bounded, atomic protected-file store to call DPAPI under Wine. Keep the same
versioned envelope, application entropy, integrity checks, memory wiping, and secret-free ordinary
settings. Detect Wine and show a persistent options warning that compatibility records are encrypted
but do not have native Windows user-scoped protection.

Do not describe Wine compatibility storage as secure or equivalent to native DPAPI. Do not fall back
to plaintext, ordinary JSON, embedded-key encryption, or Wine Credential Manager. Native Windows
continues to use normal user-scoped DPAPI without the compatibility warning.

## Consequences

- DonBot verification and Twitch session persistence can operate under Wine/Proton.
- Users accept a weaker copied-profile/local-attacker threat boundary under Wine.
- Existing native Windows records and file formats remain unchanged.
- Windows tests continue to require DPAPI round-trip and tamper rejection; Wine validation must also
  prove a real protect/store/load round trip.
- A future host-keyring bridge can replace this compatibility mode without changing provider or
  settings contracts.
