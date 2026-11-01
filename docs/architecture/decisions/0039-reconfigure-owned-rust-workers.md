# ADR 0039: Reconfigure workers through their application owners

- Status: Accepted
- Date: 2026-10-16

## Context and decision

Full runtime composition must apply ordinary settings to workers that are already owned by the
upload and metadata coordinators. Exposing those workers to rendering or creating another mutable
owner would weaken the established boundary.

Add an application-owned provider configuration value containing parallelism and one typed provider
policy. UploadCoordinator routes that value to its existing provider port and contains adapter
panics. Each production provider validates its policy, grows or shrinks its own pool, then publishes
the new policy. Invalid policy, unsupported parallelism, resource failure, and shutdown do not publish
replacement policy. Queued and active requests retain their captured values. No configuration action
dispatches a log or changes job state. Adapters without this capability explicitly return unavailable.

Add parser-capacity control to LogMetadataParser and route it through LogIngestionCoordinator. The
production metadata worker enforces the settings range of 1 through 64. Application calls contain
adapter panics and reject changes after shutdown.

Worker queue capacity now lives under the existing queue mutex. Growth reserves both input and
output storage before publishing the limit. Failure can increase allocated storage but preserves the
previous admission limit. Shrinking retains accepted requests and completed results. Admission and
result publication use greater-than-or-equal bounds, and available capacity saturates at zero until
the existing backlog drains. Growth wakes blocked result publishers. No resize detaches, cancels,
restarts, or replaces accepted work. Existing pool ownership and joined destruction remain unchanged.

## Validation and remaining integration

Tests cover provider routing, captured queued policy, invalid-policy rollback, independent
parallelism, parser input shrink, output-backlog shrink, growth wakeups, failed allocation, panic
containment, and shutdown. Tests use fake providers and controlled worker barriers without public
network requests or filesystem changes.

These ports remove two runtime-composition gaps. Startup restoration, ordered Client ID application,
periodic verification refresh, runtime scheduling, native rendering, and full release gates remain.
