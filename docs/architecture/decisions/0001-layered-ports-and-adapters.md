# ADR 0001: Layered ports-and-adapters architecture

- Status: Accepted
- Date: 2026-08-19

## Context

The addon must combine Nexus callbacks, an immediate-mode UI, filesystem notifications, EVTC parsing,
four HTTP integrations, OAuth token rotation, and reliable hot-unloading. Directly coupling these
mechanisms would make behavior difficult to test and shutdown ownership difficult to reason about.

## Decision

Use a domain-centered, layered ports-and-adapters architecture:

- Domain code owns provider state and policy and depends only on the standard library.
- Application code owns use cases and interfaces for external work.
- Native and service-specific code implements those application interfaces.
- Nexus entry and UI code remain adapters at the outer edge.
- Mutable jobs have one owner; asynchronous components communicate through bounded queues.

## Consequences

- Core behavior can be tested without loading Guild Wars 2 or contacting services.
- External-library choices can change without rewriting job policy.
- Additional adapter and result types are required at boundaries.
- Architecture tests are primarily enforced through target dependencies, code review, and header
  placement rather than a runtime framework.

