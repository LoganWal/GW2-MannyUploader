# `.zevtc` archive contract

This contract defines the narrow ZIP behavior supported by GW2 Manny Uploader. It is intentionally
smaller than a general archive-extraction API: one stable `.zevtc` file produces one in-memory EVTC
payload or one typed failure.

## Input identity

The reader receives the canonical path, size, and last-write observation emitted by log discovery.
It opens the path through `std::filesystem::path` and requires the current byte size to equal the
discovered size. A missing, unreadable, or changed file is `FileUnavailable`; the watcher may observe
the replacement as a new identity later.

The adapter does not trust the filename extension as proof that the bytes are a ZIP archive.

## Entry selection

- The central directory must contain no more than 16 entries by default.
- Exactly one non-directory entry must end in `.evtc`, matched with ASCII case folding.
- Other entries are ignored but still count toward the archive entry limit.
- A missing or second matching entry makes the archive invalid.
- Encrypted entries and compression methods unsupported by the pinned reader are rejected.
- The selected EVTC entry must not be empty.
- Entry filenames are limited to 1 KiB by default.

Nested entry names are allowed. No archive entry is ever written to disk, so path traversal sequences
have no filesystem target.

## Resource limits and integrity

Defaults are configured when the reader is constructed:

| Resource | Default limit |
| --- | ---: |
| Archive file | 256 MiB |
| Uncompressed EVTC entry | 768 MiB |
| Central-directory entries | 16 |
| Entry filename | 1 KiB |
| Uncompressed/compressed ratio | 200:1 |

All configured limits must be non-zero. The reader checks advertised sizes and expansion ratio before
allocating the output buffer. It extracts in 64 KiB chunks and requires miniz iterator finalization to
succeed, which completes output-size and CRC validation. Truncated archives, invalid central/local
metadata, malformed deflate streams, and CRC mismatches are rejected.

The current application deliberately buffers the bounded EVTC payload because the metadata decoder
requires random access to the agent and combat-event tables. The bound is part of the public adapter
contract and must not be raised casually.

## Cancellation

An already-stopped token returns `Cancelled` before opening the file. During extraction, cancellation
is checked before every output chunk. The archive reader performs no detached work and owns all ZIP
state through stack-bound guards.

## Error categories

| Code | Meaning |
| --- | --- |
| `FileUnavailable` | The path cannot be read or no longer has the discovered size. |
| `InvalidArchive` | ZIP structure, entry selection, compression support, size, or CRC is invalid. |
| `ResourceLimit` | A configured archive, entry, filename, count, ratio, or allocation bound is exceeded. |
| `Cancelled` | Cooperative cancellation was requested. |

After successful extraction, EVTC binary validation has its own error categories as documented in
[`evtc-metadata-subset.md`](evtc-metadata-subset.md).

## Asynchronous parser adapter

`MetadataParserWorker` adapts the synchronous archive/metadata reader to `ILogMetadataParser`:

- one owned `std::jthread` processes requests in FIFO order;
- input and completed-result queues have the same fixed, non-zero capacity;
- a full input queue rejects dispatch rather than growing or blocking the caller;
- every result preserves the submitted `UploadJobId`;
- reader failures remain typed, while exceptions are contained and converted to `Internal`;
- result consumers may poll or wait with a timeout; and
- cancellation rejects new work, clears queued requests/results, requests the active parse to stop,
  and joins during destruction.

The active parse is not counted in `pending_count()`. An output queue at capacity backpressures the
worker until the application consumes a result or cancellation begins.
