# Asynchronous provider-worker contract

`AsyncUploadWorker` is the shared bounded execution mechanism used by synchronous HTTP provider
adapters. It owns concurrency and queue behavior; small provider wrappers retain protocol-specific
request validation, credential ordering, result mapping, and user-facing generic details.

## Ownership and queues

Each worker owns a bounded pool of `std::jthread` instances, a bounded FIFO request queue, and an
equally bounded result queue. Queue capacity must be non-zero and defaults to eight. Parallelism is
independently configurable per provider from 1 through 32 and defaults to one. Its borrowed request
processor must outlive the worker. The provider wrapper owns the worker and destroys it before its
client, protected store, or processor state can disappear.

Common enqueue validation requires a known matching provider ID, non-zero stable job ID and attempt,
and non-empty path. Provider wrappers enforce payload-specific rules before delegation. A full,
stopping, or malformed request is rejected synchronously with a generic diagnostic.

Requests leave the queue in FIFO order, with up to the configured number active at once; results are
published in completion order. A full result queue blocks only provider worker threads until the
application takes a result. Each provider owns its own pool, so a setting of ten allows ten active
dps.report requests and a separate ten for every other provider. It never creates detached tasks,
unbounded queues, or one thread per upload. Results preserve job/provider identity and remain subject
to the coordinator's validation.

Parallelism may change while the addon remains loaded. Increasing it starts and owns the additional
threads before publishing the new limit. Decreasing it immediately lowers admission for later work;
already-active operations finish and surplus owned threads remain dormant until shutdown. A failed
or out-of-range update preserves the previous effective limit.

## Failure and cancellation

The provider processor receives the worker's stop token. An escaping processor exception becomes a
provider-specific generic failed result and cannot terminate the process or expose exception text.
Queue-allocation failure stops the worker and discards pending and unconsumed results.

`cancel_pending()` is idempotent. Its first call marks the worker stopping, clears both queues,
requests cooperative stop from every owned thread, and wakes every wait. No result is published after
stop. Destruction calls cancellation and joins the pool. The coordinator separately settles all still-waiting, active, or
retry-scheduled job states as cancelled.

The dps.report, GW2Wingman, and DonBot wrapper suites exercise the shared worker with condition-variable
fakes. Coverage includes FIFO ordering, stable IDs, bounded input, result backpressure, retry mapping,
exception containment, configurable parallel admission, in-flight stop observation, pending-work
removal, late enqueue rejection, idempotent cancellation, and joined destruction. DonBot additionally
freezes ordinary configuration at enqueue time and loads its protected key only when the attempt
starts.
