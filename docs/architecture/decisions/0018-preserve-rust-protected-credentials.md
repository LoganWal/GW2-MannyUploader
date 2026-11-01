# ADR 0018: Preserve protected credentials in Rust

- Status: Accepted
- Date: 2026-10-16

## Context

Migration must preserve the existing protected records without putting credentials into ordinary
settings or introducing a storage fallback. Native Windows and Wine both require working credential
persistence, with the reduced-protection Wine mode described by ADR 0007.

## Decision

The application owns SecretValue, the three stable SecretId values, typed redacted errors, and the
SecretStore and SecretProtector ports. SecretValue adopts an owned byte allocation, exposes bytes
only through an explicit method, and has no Clone, Display, or serialization implementation. Debug
always emits a fixed redaction marker. Clear and drop wipe the backing allocation.

Use pinned zeroize 1.8.2 with only its alloc feature, under the MIT license. Its vector implementation
wipes the complete capacity using volatile operations and compiler fences. Its Rust 1.72 minimum
fits this workspace. Keep the portable crate's unsafe-code prohibition and the domain's standard
library boundary. Envelope construction allocates its complete length before copying plaintext to
avoid leaving reallocated buffers behind. Wiping reduces owned-memory lifetime without promising
that operating-system, allocator, parser, or HTTP-library internals never copy secrets. See the
[zeroize documentation](https://docs.rs/zeroize/1.8.2/zeroize/).

The configuration adapter preserves the existing binary envelope: eight magic bytes MNYSECR1,
little-endian u32 version 1, little-endian u32 ID, little-endian u32 payload length, payload bytes,
then little-endian CRC-32 of all preceding bytes. IDs 1, 2, and 3 retain the dps-report-token.bin,
donbot-gw2-api-key.bin, and twitch-oauth-session.bin filenames. Invalid numeric IDs cannot construct
the Rust enum. Values remain bounded to 1 through 16 KiB, protected files to 64 KiB, and protector
plaintext to 32 KiB. The envelope CRC detects corruption and substitution, not authenticity.

The store composes the existing bounded FileStore with a required protector. It validates before
protection or writing, uses flushed atomic replacement, keeps no backup, and erases both primary and
stale temporary files. Errors contain only enums, an optional stable ID, and an optional numeric
system code. No error carries a path, credential substring, or platform message.

The native crate isolates CryptProtectData and CryptUnprotectData with user scope, UI forbidden,
no prompt, and the existing application entropy GW2-MannyUploader/secret-v1. That public entropy is
format binding, not a key. A unique allocation guard wipes every DPAPI output before LocalFree,
including output-limit and error paths. Input and output bounds are checked before copying. Follow
the Microsoft contracts for
[CryptProtectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata)
and [CryptUnprotectData](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptunprotectdata).

Detect Wine using the ntdll wine_get_version export and expose WineCompatibility separately from
NativeUserScoped. Keep the same protection format and functionality in both modes. No fallback is
provided. Runtime configuration and UI composition must preserve the compatibility warning.

## Verification

Portable tests cover every ID, fixed filenames, empty and maximum values, missing records, envelope
layout, every truncated prefix, byte tampering, invalid versions and IDs, record substitution, size
caps, protector failures, file failures, retry after failed replacement, no backups, and erasure.
A compile-fail example verifies that SecretValue cannot be cloned. Only synthetic markers are used.

Native tests require successful real DPAPI round trips and tamper rejection. They accept the existing
MANNY_REQUIRE_NATIVE_DPAPI and MANNY_REQUIRE_WINE_DPAPI assertions and never silently skip unavailable
DPAPI. Real long Unicode paths cover replacement failure while locked, old-record preservation,
temporary cleanup, corruption recovery, and disconnect erasure.

The native tamper fixture changes the final protected byte, both at the protector boundary and
through the real file store. This matches the existing C++ validation contract. A midpoint mutation
of the small fixture hits an unused upper key-length byte in Wine 11's metadata, leaving the
decrypted payload unchanged. It therefore does not test payload integrity. The trailing fingerprint
mutation must fail, and restoring the original file must restore loading. This changes no storage
format or protection policy. See the
[Wine 11 DPAPI implementation](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/crypt32/protectdata.c).

An ignored synthetic probe links the genuine MSVC C++ core and writes all three credential records.
Rust loads each through the real DPAPI adapter and writes replacements. The existing C++ reader
decrypts and compares every Rust record successfully. No installed credentials are accessed.
Hosted native release checks remain required. Wine validation is optional under
[ADR 0048](0048-retire-wine-validation.md).
