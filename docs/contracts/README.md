# Contracts

This directory contains independently written format and adapter contracts. External-provider
request/response specifications will be added here before their clients are implemented.

Current contracts:

- [`evtc-metadata-subset.md`](evtc-metadata-subset.md): the revision-1 EVTC fields required for boss
  ID and point-of-view account metadata;
- [`zevtc-archive.md`](zevtc-archive.md): ZIP entry selection, integrity, resource limits,
  cancellation, and asynchronous parser behavior.
- [`log-polling-pipeline.md`](log-polling-pipeline.md): portable filesystem polling, snapshot
  reconciliation, bounded application ticks, and shutdown.
- [`settings-schema-v1.md`](settings-schema-v1.md): ordinary settings, validation, secret separation,
  atomic persistence, and last-known-good recovery.
- [`protected-credentials.md`](protected-credentials.md): stable secret identifiers, redaction,
  atomic protected records, native Windows DPAPI policy, and warned Wine compatibility behavior.
- [`configuration-service.md`](configuration-service.md): application ownership, startup capability,
  secret-free snapshots, durable write-through updates, errors, and shutdown.
- [`nexus-options.md`](nexus-options.md): render-safe snapshots, bounded configuration commands,
  workflow-owned DonBot/Twitch controls, view-model state, redaction, and shutdown.
- [`recent-log-actions.md`](recent-log-actions.md): render-safe open-report/open-folder commands,
  explicit retry/reupload/rechat behavior, trusted-target validation, backpressure, and shutdown.
- [`upload-history.md`](upload-history.md): bounded durable log identity and provider receipt state,
  restart normalization, discovery seeding, and no-automatic-replay policy.
- [`http-transport.md`](http-transport.md): provider-independent requests, streaming bodies,
  timeouts, cancellation, redirects, response limits, TLS policy, and secret redaction.
- [`dps-report.md`](dps-report.md): multipart log upload, protected user tokens, response validation,
  retry classification, long processing timeouts, and deterministic provider tests.
- [`dps-report-provider-worker.md`](dps-report-provider-worker.md): bounded off-thread execution,
  result delivery, credential-rotation ordering, retry scheduling, cancellation, and joined shutdown.
- [`wingman.md`](wingman.md): direct raw-EVTC compatibility upload, duplicate semantics, response
  validation, retry classification, compatibility risk, and deterministic provider tests.
- [`donbot.md`](donbot.md): protected-key verification, authorized guild discovery, strict TUS
  creation/location/PATCH behavior, `wingman=false`, and duplicate-safe retry policy.
- [`donbot-provider-worker.md`](donbot-provider-worker.md): configuration snapshotting, per-attempt
  protected-key loading, result mapping, and shared bounded-worker behavior.
- [`donbot-configuration-workflow.md`](donbot-configuration-workflow.md): asynchronous key
  verification, transient identity, fail-safe persistence ordering, guild selection, disconnect, and
  shutdown.
- [`twitch.md`](twitch.md): public-client Device Code OAuth, validation and rotating-token rules,
  broadcaster-owned Helix chat delivery, retry classification, and secret handling.
- [`twitch-authentication-workflow.md`](twitch-authentication-workflow.md): protected session format,
  bounded primitive worker, Device Code scheduling, atomic refresh persistence, startup, disconnect,
  and secret-free connection snapshots.
- [`twitch-chat-delivery.md`](twitch-chat-delivery.md): version-1 templates, same-job delivery,
  session access, one 401 recovery, receipts, retry ambiguity, and duplicate suppression.
- [`async-provider-workers.md`](async-provider-workers.md): shared bounded FIFO execution,
  backpressure, exception containment, cancellation, and joined shutdown for provider adapters.

Each provider contract records:

- base URL and endpoint;
- HTTP method, query, required headers, and body fields;
- authentication and secret-redaction rules;
- success, duplicate, retryable, and permanent-failure responses;
- response-size and timeout limits;
- cancellation behavior;
- deterministic fake-server test cases;
- date and source used to verify the contract.

Contracts describe public APIs and observed behavior. They do not contain copied implementation code
from the previous uploader.
