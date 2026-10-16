# ADR 0024: Upload to Wingman through Rust workers

- Status: Accepted
- Date: 2026-10-16

## Decision

WingmanClient implements the frozen Wingman contract through injected HTTP, random-source, and
monotonic polling ports. With a same-job dps.report receipt, it imports the trusted permalink and
checks the returned queue/database status without opening the archive. Otherwise it streams the
stable archive and bounded account metadata to the compatibility bridge. The bridge endpoint stays
adapter-owned, with a validated constructor seam for a reviewed replacement.

Direct uploads preserve multipart field order, long upload timeouts, duplicate handling, and legacy
success without a ticket. Ticket processing polls every three seconds for up to thirty minutes.
Permalink discovery polls every five seconds for up to five minutes. Each wait and HTTP operation
is capped by the remaining phase time. Expiry preserves accepted delivery without inventing a fight
link. Cancellation interrupts waits and requests, and is checked again before accepting HTTP results.
The application-owned PollClock permits deterministic deadline tests without wall-clock delays.

Provider JSON helpers now share bounded object/text decoding and the existing Unicode escape
validation. Unknown fields remain forward compatible. Response schemas, safe Wingman slugs, exact
trusted report origins, percent encoding, and bounded Retry-After values are checked before use.
Errors retain only typed categories, status codes, and retry delays. Responses, accounts, paths,
request URLs, and panic messages do not enter diagnostics.

WingmanProviderWorker composes the existing AsyncUploadWorker. Stable job and attempt IDs, bounded
queues, per-provider concurrency, panic containment, cancellation, and joined shutdown remain owned
by the shared pool. The coordinator remains responsible for WvW exclusion with zero attempts.
The worker and direct client also reject WvW inputs before network work.

UploadOutcome's typed success warning becomes a success notice so queued imports and existing
uploads can carry static status text alongside validated receipts. Duplicate delivery reports
Already present. New imports report Queued. The existing dps.report warnings retain their text and
receipt/persistence ordering. No provider can inject arbitrary response text into these notices.

## Verification and remaining work

Fake HTTP tests cover both routes, exact streamed bytes, Unicode accounts, endpoint and response
limits, queue/database combinations, ticket states, duplicate and legacy success, invalid responses,
trusted targets, phase deadlines, transport/status retry rules, file mutation, random-source failure,
cancellation, and panic containment. Fake time covers the full polling windows without public uploads.
Worker tests cover route selection, receipt/notice mapping, bounded retries and admission, invalid
requests, WvW exclusion, cancellation, and joined destruction. Coordinator tests preserve visible
notices and fight receipts without dispatching again.

The native DLL lifecycle shell does not yet compose these providers. DonBot, Twitch, authentication,
production runtime/UI composition, active-work unload tests, Wine execution, packaging, and hosted
release gates remain required before replacing the production C++ addon. No test uploads a log to a
public service.
