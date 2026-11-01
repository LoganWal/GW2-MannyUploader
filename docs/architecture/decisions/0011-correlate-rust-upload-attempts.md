# ADR 0011: Correlate Rust upload attempts and retain replay intent

- Status: Accepted
- Date: 2026-10-16

## Context

The Rust migration needs the application coordinator before adding provider workers. Existing
contracts require independent provider admission, same-job report dependencies, bounded work,
explicit replay, and no automatic resend after ambiguous Twitch delivery.

A completion identified only by job and provider can accidentally settle a later attempt. A single
user-initiated flag also cannot distinguish failed-provider retry from Reupload, which must suppress
DonBot Discord delivery through subsequent retry attempts.

## Decision

The portable application layer owns clock and provider traits. One coordinator owns mutable jobs
and boxed provider ports. Requests own their file identity, metadata, and optional same-job report.
Snapshots own copies of job records and expose no mutable coordinator references.

Correlate every completion with job ID, provider, and nonzero attempt number. Accept it only while
that exact attempt is active. Poll results round-robin with a per-call budget and check that the
result's provider matches its source port. Preserve Automatic, ManualRetry, Reupload, or Rechat
intent across scheduled retries. Reupload retries rearm only upload dependencies and preserve Twitch.

Use typed success payloads and distinct skipped, retry, failure, ambiguous, dropped-chat, and cancelled
outcomes. Only an explicit safe-retry outcome schedules a monotonic deadline, bounded to 24 hours.
Ambiguous results settle as failed. The application generates diagnostics from fixed local strings.
Provider adapters remain responsible for protocol validation and deciding whether a retry is safe.

Validate results and grouped Reupload preparation on a job copy before committing. Validate new jobs
before consuming an ID or evicting history. Restore the configured tail transactionally without
dispatch, normalizing interrupted states through the domain model. Permit history limits from 1
through 10,000, matching the durable history ceiling. Shrinking preserves busy jobs until they settle.
The persistence adapter must validate the entire document and seed discovery before tail selection.

Enforce report dependencies again on manual retry, including restored jobs. A failed or interrupted
selected dps.report destination cannot silently turn a dependent retry into a direct archive upload.
Allow an active provider to transition to Skipped because provider contracts can discover an
unsupported or ineligible request after dispatch.

Provider enqueue, polling, and cancellation panics are contained. Cancellation is idempotent and
settles busy jobs. The coordinator requests cancellation on destruction. Each future worker adapter
must own bounded queues, implement nonblocking port calls, and join all owned workers on destruction.

## Consequences

The coordinator uses only the standard library and is exercised with fake clocks and providers.
No provider performs real uploads in this slice. The native development DLL remains a lifecycle
shell until the bounded worker pool, metadata pipeline, and application owner are integrated.

This adds attempt validation, atomic admission, and explicit replay intent beyond the C++ coordinator
while preserving the frozen product behavior. Persistence schema compatibility, provider protocol
checks, credential handling, and joined threaded shutdown remain separate migration gates.
