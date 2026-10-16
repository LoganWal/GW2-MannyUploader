# ADR 0031: Implement Twitch HTTP primitives in Rust

- Status: Accepted
- Date: 2026-10-16

## Decision

The application owns the synchronous TwitchApi port, move-only authorization/grant values,
validated identity, and typed operation/error classifications. TwitchClient implements that port
with the shared HTTP adapter. It performs one network primitive per call and owns no scheduling,
persistence, retry, refresh chaining, or chat recovery.

The client implements public-client Device Code start and polling, access validation, rotating
refresh grants, revocation, and broadcaster-owned Helix chat. Requests contain no client secret.
One validated public Client ID is captured per operation. Replacing configuration releases its
mutex before HTTP, so an in-flight operation retains its original client binding. The future
application controller restricts replacement to disconnected/error states and supplies the normal
packaged Client ID fallback.

OAuth forms compute their complete encoded length from bounded values, then fill one SecretValue
allocation. Sensitive Authorization headers are also filled directly into wiping storage. HTTP
response bodies and decoded token/device-code fields retain wiping ownership. JSON visitors bound
secret strings before copying into SecretValue instead of retaining ordinary owned token Strings.
The shared JSON adapter enforces object shapes, UTF-8 and paired Unicode escapes, field uniqueness,
bounded lists, and the 64 KiB response cap. As with the other serde-based providers, serde's internal
temporary parsing storage is library-owned.

Device authorization checks the exact HTTPS Twitch activation path, positive bounded expiry and
polling interval, and bounded visible user/device codes. Polling recognizes only the exact 400
authorization_pending envelope. Grants accept the single chat scope as either a string or one-item
array and require a Bearer token type. Validation binds the exact captured Client ID, canonical
user ID, valid login, positive expiry, and one chat scope.

Chat accepts one authenticated user ID and uses it for both broadcaster and sender. It checks the
expanded 500-code-point and 2,000-byte message limits and omits for_source_only. The response must
contain exactly one consistent sent or dropped item. Sent message IDs satisfy the existing durable
receipt's visible-ASCII bounds. Drop code/message fields are validated, then reduced immediately to
the existing normalized TwitchDeliveryStatus. Raw server reasons never cross the application port.

All operations use ten-second connect and thirty-second operation/stall timeouts. Existing HTTP
header/body hard limits still apply, including the Authorization scheme prefix. Connectivity and
selected status failures retain bounded retry metadata. Validation/chat 401 and refresh 400/401
require reconnection. Invalid validation identity also requires reconnection. Cancellation and
adapter panics become typed outcomes without retaining payloads or server text. The client never
acts on a retry suggestion itself.

## Verification and remaining work

Fake-transport tests assert exact URLs, forms, headers, timeouts, response limits, sensitive-header
metadata, device and grant fields, scope and identity binding, chat JSON, normalized drop codes,
Unicode boundaries, truncation, malformed/oversized responses, status and transport categories,
Retry-After rules, cancellation, panic redaction, configuration admission, and concurrent Client ID
replacement. Tests never contact Twitch or send real chat messages.

The authentication worker/controller, shared session owner, chat recovery/delivery, test-message
workflow, UI, and native runtime composition remain pending. Wine, packaging, symbols, active-work
DLL unload, and hosted release gates remain open.
