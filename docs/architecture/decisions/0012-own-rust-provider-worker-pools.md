# ADR 0012: Own bounded Rust provider worker pools

- Status: Accepted
- Date: 2026-10-16

## Context

The Rust coordinator needs an off-thread execution adapter under the existing asynchronous worker
contract. Providers need independent concurrency, bounded request and result queues, live limit
changes, and cooperative cancellation. No worker may outlive addon unload.

## Decision

Add a standard-library provider pool implementing the application-owned UploadProvider port. Each
pool owns its processor, shared queue state, and every JoinHandle. Processors are Send, Sync, and
owned for the pool lifetime. Processing runs outside the queue mutex and returns only UploadOutcome.
The pool constructs job, provider, and attempt identity from the accepted request.

Preallocate both bounded FIFO queues and storage for at most 32 thread handles before starting any
thread. Reject allocation failure during construction. Queue push/pop operations then need no
allocation. Queue capacity defaults to eight and parallelism defaults to one. The pool performs common
request validation. Provider wrappers will add protocol validation and configuration capture before
admission, and load protected credentials when processing starts.

Count an attempt as active until its result is published. When the result queue is full, completed
attempts retain their admission slots and wait on a condition variable. Bounded completion tickets
preserve publication order across these waits, including counter wrap. Taking a result wakes blocked
publishers. Enqueue and nonblocking result retrieval never wait for network work or queue space.

Allow parallelism changes from 1 through 32. Start and own additional threads before publishing an
increased limit. A failed increase preserves the old limit and retains any partially started threads
as dormant owned workers. Decreasing the limit immediately stops excess admission while active
attempts finish. Surplus threads remain dormant and can be reused by later increases.

The application layer owns a cancellation source and a clonable read-only StopToken. The token
supports cheap stop checks and an interruptible timed wait. Pair cancellation notification with its
wait mutex to prevent lost wakeups. Processors must cooperate with stop and bound external waits.

Share the internal typed queue core with the single-thread metadata parser adapter. The core owns
only queue, thread, cancellation, and completion-order mechanics. Provider and metadata wrappers
retain their application-owned ports, request validation, result identity, and typed panic outcomes.
Metadata ingestion checks available input capacity before allocating a job and tracks admitted IDs
until completion. Pending metadata counts as busy even when all provider destinations are disabled.

Cancellation marks the pool stopping, clears queued requests and results, requests processor stop,
and wakes queue waiters. Later completions are discarded. Drop cancels and joins every handle before
releasing processor state. Failed construction uses the same cleanup path. No thread is detached.

Contain processor panics and publish a generic failed outcome without the panic payload. This never
schedules an automatic retry. Native integration must retain ADR 0010's panic-output suppression
until every owned worker has joined, because Rust invokes the panic hook before catch_unwind.

## Verification and consequences

Tests coordinate processors with channels and condition variables. They cover FIFO and request
identity, Unicode paths, both queue bounds, independent provider admission, completion ordering,
parallelism changes, partial thread-start failures, panic containment, stop wakeups, cancellation
under result backpressure, and processor lifetime after joined destruction. A coordinator integration
test uses the real pool to verify owned cancellation and shutdown.

The pool adds no dependencies or unsafe code. The metadata adapter now connects the archive reader
and decoder through the shared pool. Provider protocols, credentials, discovery, and the background
application owner remain separate adapters. Native worker hot-unload validation remains pending
until runtime composition connects these workers to the DLL.
