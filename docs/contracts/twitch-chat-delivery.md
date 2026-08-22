# Twitch message rendering and chat-delivery contract

This contract freezes version 1 of both application paths to a broadcaster-owned Twitch chat attempt:
an accepted dps.report result and an explicit options-page test message. It complements the HTTP wire
contract in [`twitch.md`](twitch.md) and the credential lifecycle in
[`twitch-authentication-workflow.md`](twitch-authentication-workflow.md).

## Template grammar

An ordinary-settings template is non-empty valid UTF-8, contains no ASCII control character, and is
at most 500 encoded bytes. Text is literal except for:

- `{{` and `}}`, which render one literal brace;
- `{url}` for the dps.report permalink;
- `{encounter}` for the dps.report encounter name;
- `{mode}` for the dps.report mode verbatim;
- `{mode_suffix}`, which is empty for an empty mode and otherwise ` (` + mode + `)`;
- `{result}`, which is exactly `Success` or `Failure`; and
- `{boss_id}`, which is the unsigned decimal dps.report boss ID.

Every single brace begins or ends a placeholder. Empty, nested, unknown, unmatched, and unclosed
placeholders are errors. At least one real `{url}` placeholder is required; escaped `{{url}}` does
not satisfy that rule.

`TwitchMessageTemplate::parse` is the sole parser used by settings validation and delivery. Rendering
uses only the same-job `DpsReportResult`; it does not consult a filename, independently parsed name,
or mutable global settings. Every substituted field must be valid UTF-8 without ASCII controls. The
final message must be non-empty, at most 2,000 encoded bytes, and at most 500 Unicode code points.
The code-point limit is checked after all expansion, so a source template passing its 500-byte limit
does not imply that any particular report can be posted.

## Request and configuration capture

The upload coordinator is the sole creator of Twitch job requests. It dispatches Twitch only after
the same job has completed dps.report with a non-empty result. `TwitchProviderWorker::enqueue` rejects
a request without that result or with any foreign provider context.

The wrapper snapshots the current template and success/failure posting policy into the accepted
request before it enters the worker queue. Later options changes affect new requests only. A report
excluded by that captured policy produces `UploadOutcome::Skipped` without acquiring a credential,
rendering an HTTP request, or touching the delivery ledger.

## Session-access boundary and 401 recovery

`ITwitchDeliverySessionAccess` is the only credential boundary visible to the chat worker. An acquired
lease contains a move-only access token, authenticated user ID, and opaque session revision. It does
not contain a configurable channel or sender. `TwitchSessionOwner` is the one concrete process-local
owner behind that boundary and the authentication controller. The chat worker never reads
`ISecretStore`, decodes protected records, refreshes tokens, or mutates connection snapshots directly.

The owner exposes a connected session only when no mutation is active. Saved-session validation, a
new grant, scheduled validation, refresh, disconnect, and chat-side recovery each hold one exclusive
transaction. While a transaction is open, lease acquisition and competing recovery return a typed
retry result; a controller checkout cannot observe or overwrite an intermediate generation. A
completed transaction atomically stores the complete session, advances the process-local revision,
and only then publishes the new lease. Cancellation, terminal failure, and shutdown invalidate the
transaction before another lease can be acquired.

After one chat `401`, the worker transfers the rejected lease to `recover` exactly once. If its
revision is older than the active revision, the owner returns the newer lease without another refresh.
Otherwise the owner validates the rejected generation under an exclusive transaction. A reconnect
result permits one refresh; the rotated complete token pair is atomically protected before replacement
validation and is published only after the same broadcaster identity is confirmed. The worker sends
the same rendered message once more with that replacement. A second `401` fails with a
reconnect-required diagnostic. No request gets a second recovery cycle.

The session access is borrowed and must outlive the provider. It must be safe to call on the provider
thread and must observe the supplied stop token. Its diagnostics follow the same no-secret policy as
the Twitch client.

`TwitchChatDelivery` is the common synchronous policy boundary used by encounter posts and explicit
test messages. It owns neither a thread nor credentials. Given an already rendered message, it
acquires the broadcaster lease, sends as that same authenticated user, applies the one-`401` recovery
cycle, normalizes sent/drop/retry/failure/cancellation results, and identifies ambiguous outcomes.
Provider-specific job and deduplication state remains outside this common boundary.

## Explicit options test message

`SendTwitchTestMessageCommand` is accepted by the application path only while the broadcaster session
is publicly `Connected` and no earlier test request is in flight. Application execution assigns a
non-zero monotonically increasing request ID and queues only that ID through `ITwitchTestMessenger`.
It does not manufacture an `UploadJobId`, log identity, encounter metadata, dps.report result, or
permalink.

`TwitchTestMessageWorker` renders the fixed local text
`GW2 Manny Uploader test message #<request-id>` and sends it through `TwitchChatDelivery` on one
bounded joined thread. Including the explicit request ID makes consecutive user-requested tests
distinct under Twitch's duplicate-message policy. The ordinary encounter template and result-posting
switches do not affect this text.

The controller correlates the returned request ID before publishing `Sent` or `Error`. Sent results
expose the normalized `Sent` status but do not expose the remote message ID. Drop details are locally
defined, and ambiguous failures use the common generic diagnostic. A safe-retry result is shown as an
error; the options controller never schedules another chat attempt. The user may deliberately press
the enabled test control again after the prior request becomes terminal.

## Sent, dropped, and correlated results

A successful send records a normalized `TwitchDeliveryReceipt` with `Sent` and the bounded message ID.
The coordinator accepts Twitch success only with that receipt and preserves it in the job snapshot.

`is_sent: false` is a completed Helix exchange but a failed delivery. Known drop codes normalize to
AutoMod, blocked term, duplicate, rate limited, followers-only, slow mode, subscribers-only, or
restricted. Unknown future codes normalize to `OtherDrop`. The receipt contains no message ID, and
the displayed detail is locally defined; Twitch's raw server message is not copied into job state or
logs.

## Retry and duplicate-risk policy

Automatic retry is allowed only when the worker can establish that chat delivery did not begin:

- name resolution failure;
- connection failure;
- TLS setup failure; or
- an explicit HTTP `429` rejection.

The returned delay is positive and capped at 24 hours, otherwise it becomes 30 seconds. A `401` uses
the single recovery path instead of a coordinator retry.

Timeouts, send failures, receive failures, cancellation after entering the client, malformed `200`
responses, response-limit failures, HTTP `408`, and `5xx` responses are ambiguous: Twitch might have
accepted the message even though the addon did not obtain a trustworthy receipt. They become a
permanent failed result with a generic diagnostic, never an automatic retry.

The worker retains a bounded 256-entry ledger keyed by stable job ID plus permalink. A confirmed sent
key returns its prior receipt without another client call, and an ambiguous key refuses another
automatic attempt. Old entries leave from the front only when the bound is reached. A request marked
as an explicit user-initiated action bypasses either entry so `Rechat` can deliberately send again.
The ledger is mutex-protected because multiple Twitch deliveries may be active concurrently. Durable
job history prevents automatic dispatch after restart; the ledger remains a narrower process-local
guard for in-session ambiguity. The complete manual-action boundary is in
[`recent-log-actions.md`](recent-log-actions.md).

## Worker ownership and shutdown

The encounter provider uses the configurable shared `AsyncUploadWorker` pool. The test-message
adapter uses an equivalent narrow single-thread worker because its request and result deliberately
are not upload-job types. Both have bounded FIFO input and output queues, output backpressure,
exception containment, cooperative cancellation, and idempotent joined shutdown. The client and
session-access dependencies are borrowed and must outlive them. Neither render callbacks nor the
coordinator application thread performs HTTP or protected-credential work.

## Deterministic coverage

Tests cover all six fields, brace escapes, every parser error class, invalid substituted values,
empty-mode suffixes, success/failure wording, exact 500-code-point Unicode messages, byte overflow,
policy skips, configuration capture, same-user rendering, every normalized drop class, one recovery,
second-401 failure, stale-lease handoff, serialized controller/recovery races, identity mismatch,
shutdown during recovery, session failures, bounded safe retries, every ambiguous class, posted and
ambiguous dedupe, cancellation, and wrapper validation without contacting Twitch. Test-message
coverage additionally proves fixed unique text, broadcaster identity/token use, request correlation,
no automatic ambiguity retry, queue and output backpressure, exception containment, and joined
shutdown.
