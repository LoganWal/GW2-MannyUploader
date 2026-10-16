# ADR 0016: Own bounded Rust atomic file storage

- Status: Accepted
- Date: 2026-10-16

## Context

Settings, upload history, and protected records all require bounded reads and flushed atomic writes.
Platform replacement details must remain outside the safe Rust core. Failed replacement must preserve
the primary record, and disconnect must attempt removal of both primary and interrupted temporaries.

## Decision

The application owns a `FileStore` port with bounded read, atomic write, and idempotent erase methods.
Its errors contain only categories. One owner controls each persistent path, and callers must protect
credential bytes before they reach this general-purpose file port.

`FilesystemStore` uses standard file I/O and an `AtomicFileCommit` platform seam. Reads check file
type and size, allocate within the supplied limit, check growth before extending buffers, and verify
size/modification time after reading. An absent file differs from an empty file. Persistent reads and
writes have an absolute 32 MiB ceiling, with smaller caller limits for settings and protected records.

Writes create parent directories, remove an interrupted sibling `.tmp`, exclusively create the new
temporary, write and flush it, close it, then replace the primary. Removing a stale temporary before
exclusive creation prevents truncating its hard-link target or following a pre-existing symlink.
This remains a single-owner persistence adapter, not a multiprocess synchronization mechanism.
Erase attempts both primary and temporary removals even when one fails, and never removes directories.

Unix commits use rename followed by parent-directory synchronization. Windows commits live in the
native crate and call `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`, matching
the existing production adapter. They use extended-length UTF-16 paths and never enable cross-volume
copy fallback. Only the parent is canonicalized, preserving replacement of the named entry.
Windows erase retains the existing native directory-flush policy.

The flush and replacement APIs were checked on 2026-10-16 against
[Rust File::sync_all](https://doc.rust-lang.org/std/fs/struct.File.html#method.sync_all) and
[Microsoft MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).

## Consequences

Synthetic tests cover Unicode paths, missing versus empty files, exact read bounds, oversized writes,
failed replacement preserving primary bytes, interrupted-write recovery, hard-link preservation,
and cleanup when one erase fails. Native Windows tests exercise paths longer than MAX_PATH and
replacement failure while the primary is locked. The Unix suite includes real directory flushing
and stale-symlink handling when run on Linux.

These primitives do not read production data or change runtime composition. Settings backup recovery,
history compatibility, protected envelopes, DPAPI, and Wine validation remain separate work.
