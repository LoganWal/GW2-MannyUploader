# Asynchronous provider-worker contract

`AsyncUploadWorker` is the shared bounded execution mechanism used by synchronous HTTP provider
adapters. It owns concurrency and queue behavior; small provider wrappers retain protocol-specific
request validation, credential ordering, result mapping, and user-facing generic details.

## Ownership and queues

Each worker owns exactly one `std::jthread`, a bounded FIFO request queue, and an equally bounded FIFO
result queue. Capacity must be non-zero and defaults to eight. Its borrowed request processor must
outlive the worker. The provider wrapper owns the worker and destroys it before its client, protected
store, or processor state can disappear.

Common enqueue validation requires a known matching provider ID, non-zero stable job ID and attempt,
and non-empty path. Provider wrappers enforce payload-specific rules before delegation. A full,
stopping, or malformed request is rejected synchronously with a generic diagnostic.

The worker processes one request at a time. A full result queue blocks only the worker thread until
the application takes a result. It never creates detached tasks, unbounded queues, or one thread per
upload. Results preserve job/provider identity and remain subject to the coordinator's validation.

## Failure and cancellation

The provider processor receives the worker's stop token. An escaping processor exception becomes a
provider-specific generic failed result and cannot terminate the process or expose exception text.
Queue-allocation failure stops the worker and discards pending and unconsumed results.

`cancel_pending()` is idempotent. Its first call marks the worker stopping, clears both queues,
requests cooperative stop, and wakes every wait. No result is published after stop. Destruction calls
cancellation and joins the thread. The coordinator separately settles all still-waiting, active, or
retry-scheduled job states as cancelled.

The dps.report, GW2Wingman, and DonBot wrapper suites exercise the shared worker with condition-variable
fakes. Coverage includes FIFO ordering, stable IDs, bounded input, result backpressure, retry mapping,
exception containment, in-flight stop observation, pending-work removal, late enqueue rejection,
idempotent cancellation, and joined destruction. DonBot additionally freezes ordinary configuration
at enqueue time and loads its protected key only when the attempt starts.
