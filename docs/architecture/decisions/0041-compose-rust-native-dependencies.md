# ADR 0041: Compose Rust native dependencies

- Status: Accepted
- Date: 2026-10-16

## Decision

The addon composition module assembles ApplicationRuntime outside Nexus callbacks. It uses owned
absolute game/addon paths, the existing settings and history filenames, native atomic replacement,
DPAPI, Schannel HTTP, Windows random generation, directory notifications with polling fallback,
the bounded archive parser, and all provider/authentication/aggregate/test-message workers.
Construction failures drop partially assembled dependencies and join any workers already created.

Configuration and provider workers share one protected store through a mutex. Configuration owns a
forwarding adapter, while providers receive the same inner store. The adapter caches the immutable
protection mode and rejects operations after mutex poisoning. Lock order remains Twitch session
owner, configuration, protected store. A worker holding the protected-store lock never enters
configuration. No replacement credential store is created when DPAPI initialization fails. The
existing capability snapshot reports that failure while ordinary settings remain usable.

One native clock supplies wall and monotonic time to the application coordinator and workflows.
Each provider retains its own bounded worker pool and configured parallelism. History and discovery
retain up to 10,000 identities independently of the configured visible-row limit. The runtime applies
live settings and restores history through the existing application-owned ports.

Cargo accepts MANNY_TWITCH_CLIENT_ID as a public application identifier. Empty or absent values use
the existing CMake default. The build rejects invalid characters and identifiers over 128 bytes.
The runtime still permits the validated ordinary-settings override. No client secret is involved.

## Validation and remaining integration

Native tests use exclusively temporary Unicode paths and synthetic credentials. Fake HTTP exercises
the complete discovery/parser/upload/history pipeline and checks that accepted identity persistence
precedes upload, rotated credentials are protected, receipts survive restart, and unchanged logs
are not replayed. Additional tests cover durable command application, missing protected storage,
shared-store rotation/erasure, poisoned-store isolation, malformed archives, invalid construction,
and joining an authentication worker blocked in HTTP. The production Schannel composition is also
constructed and destroyed without any outgoing requests.

A native fanout scenario also verifies protected Twitch session recovery and DonBot verification,
then sends one synthetic archive through all four concrete provider workers. Wingman, DonBot and
Twitch receive the same dps.report permalink, and Twitch uses the validated broadcaster for sender
and channel. All four receipts persist across restart. Repeated discovery observations of the same
archive produce no additional delivery. HTTP remains synthetic throughout this scenario.

The exported DLL attaches this composition and its joined runtime under ADR 0042. Active-worker
DLL unload is covered by ADR 0043, and package/symbol checks by ADR 0044. Wine execution and the
native Windows/Wine in-game matrix remain required before shipping cutover.
