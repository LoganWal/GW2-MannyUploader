# Protected credential contract

This contract covers persistent credentials used by GW2 Manny Uploader. It is separate from ordinary
JSON settings: protected values must never appear in settings files, immutable UI snapshots,
diagnostics, logs, filenames, or exception text.

## Stable credential identifiers

Version 1 defines exactly three opaque records:

| Identifier | Purpose |
| --- | --- |
| `DpsReportUserToken` | Optional dps.report user token |
| `DonBotGw2ApiKey` | Guild Wars 2 API key used with DonBot |
| `TwitchOAuthSession` | One atomically replaceable Twitch OAuth session bundle |

The Twitch bundle contains access token, rotating refresh token, expiry, authenticated user ID,
login, and granted scopes in the versioned binary format frozen in
[`twitch-authentication-workflow.md`](twitch-authentication-workflow.md). Keeping it in one protected
record prevents a crash from pairing a newly rotated refresh token with stale session metadata.

Identifiers map to fixed implementation-owned filenames. No account name, channel name, token prefix,
or caller-controlled text is used in a path. Unknown enum values fail as `InvalidId`.

## Value and lifecycle rules

- A credential is an opaque, move-only byte value, not a general `std::string`.
- Values must contain 1 through 16 KiB. Empty values are erased explicitly rather than stored.
- Store replaces a record atomically. The encrypted sibling temporary file is flushed before replace.
- Secret records have no backup: keeping an old OAuth refresh token or deleted API key creates an
  unwanted second credential copy.
- Load reports `NotFound` for an absent record. It never manufactures an empty value.
- Erase is idempotent and removes both the primary record and a stale interrupted-write temporary.
- The store has one application owner and does no work from Nexus render callbacks.
- Disconnect erases the relevant persistent record after any best-effort remote token revocation.

`SecretValue` overwrites owned memory before release and cannot be copied implicitly. This reduces
ordinary lifetime and crash-dump exposure, but it cannot promise that operating-system, HTTP-library,
or provider-parser internals never make temporary copies.

HTTP headers explicitly marked sensitive also overwrite their owned value on destruction. The
Windows libcurl adapter overwrites its assembled header line and every libcurl header-list allocation
before release. This is best-effort lifetime reduction rather than a guarantee against allocator,
library, operating-system, or crash-dump copies.

## Protected file envelope

Before platform protection, each value is wrapped in a bounded binary envelope containing:

- fixed `MNYSECR1` magic;
- envelope version `1`;
- stable credential identifier;
- payload byte length;
- the opaque payload; and
- CRC-32 over the preceding envelope bytes.

The envelope detects truncation, wrong-record substitution, and corruption even if the platform API
returns output rather than a specific tamper error. CRC-32 is a corruption check, not an authentication
mechanism; native DPAPI remains responsible for confidentiality and authenticity.

Encrypted record files are capped at 64 KiB before allocation. Invalid magic, version, ID, length,
checksum, or payload limits report `CorruptRecord` without including plaintext or protected bytes.

## Platform policy

### Native Windows

Use user-scoped `CryptProtectData` and `CryptUnprotectData` with:

- `CRYPTPROTECT_UI_FORBIDDEN` and no prompt structure;
- no `CRYPTPROTECT_LOCAL_MACHINE`, so another local user is not intentionally granted access;
- fixed application entropy for domain separation, not as a substitute encryption key;
- bounded input and output; and
- prompt-free errors represented by numeric system codes only.

DPAPI output is stored beneath the addon configuration directory. Plaintext exists only in owned
memory during protect/unprotect and is overwritten before the returned Windows allocation is freed.

### Wine and compatibility layers

Wine's current `CryptProtectData` implementation does not provide Windows-equivalent credential
binding and currently ignores protection flags. Its source derives a key from the username, public
implementation constant, salt, and optional entropy. Wine Credential Manager is also unsuitable for
this use because its generic-credential key is stored in the same user registry tree as the encrypted
blob.

The DPAPI adapter therefore detects Wine through the Wine-owned `ntdll!wine_get_version` export and
returns `UnsupportedEnvironment` before accepting a credential. There is no plaintext, obfuscation,
Wine-DPAPI, or Wine-Credential-Manager fallback. Durable credentials under Wine remain unavailable
until a separately reviewed host Secret Service/KWallet bridge exists. Session-only credentials may
be considered later, but must be presented honestly as non-persistent.

## Error and redaction rules

Errors contain only a stable credential identifier, category, safe static message, and optional
numeric operating-system code. Tests use marker secrets and require that neither the value nor any
substring derived from it occurs in errors or persisted plaintext.

Required deterministic cases include create validation, all stable IDs, missing load, atomic replace,
idempotent erase, empty/oversized input, protected-record size cap, corrupt envelope, wrong-ID record,
protector failure, failed replacement preserving the old record, and stale temporary cleanup. Native
Windows CI additionally exercises a real DPAPI-backed store round trip and tamper rejection. A Wine
probe requires `UnsupportedEnvironment` and confirms that no record is written.
