# Twitch broadcaster authentication and chat contract

This contract defines version 1 of GW2 Manny Uploader's Twitch integration. It covers public-client
Device Code authentication, token validation/rotation/revocation, and chat delivery to the
authenticated broadcaster's own channel.

Verified 2026-08-20 against Twitch's official documentation for:

- [Device Code authentication](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/);
- [token validation](https://dev.twitch.tv/docs/authentication/validate-tokens/);
- [refreshing access tokens](https://dev.twitch.tv/docs/authentication/refresh-tokens/);
- [revoking access tokens](https://dev.twitch.tv/docs/authentication/revoke-tokens/); and
- [Send Chat Message](https://dev.twitch.tv/docs/api/reference/#send-chat-message).

## Application identity and scope

The addon is a public Twitch application using the Device Code flow. Its public Client ID is ordinary
user configuration, with an optional packager-provided build default. It may be replaced only while
the workflow is disconnected or in error. No client secret is compiled in, persisted, displayed,
requested from the broadcaster, or sent by this client.

The only requested and accepted scope is `user:write:chat`. The integration does not read chat, join
IRC, subscribe to EventSub, act as a bot account, or post to a separately configured channel.

## Device Code authentication

Starting a connection sends:

```text
POST https://id.twitch.tv/oauth2/device
Accept: application/json
Content-Type: application/x-www-form-urlencoded

client_id=<application-client-id>&scopes=user%3Awrite%3Achat
```

Success is exactly `200`. The bounded response must provide a non-empty device code, user code,
positive expiry, positive polling interval, and the HTTPS Twitch activation URI. The device code is a
move-only secret; only the user code, activation URI, expiry, and polling interval may enter a
secret-free connection snapshot.

Polling waits for the server-provided interval and sends:

```text
POST https://id.twitch.tv/oauth2/token
Accept: application/json
Content-Type: application/x-www-form-urlencoded

client_id=<application-client-id>&scopes=user%3Awrite%3Achat&device_code=<device-code>&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code
```

A `400` JSON error with `status: 400` and `message: "authorization_pending"` is the sole pending
result. `200` must return bounded visible-ASCII access and refresh tokens, a positive expiry,
Bearer token type, and exactly the requested scope. Every other response is a typed error.

The application layer, not this synchronous client, owns polling deadlines, authorization expiry,
cancellation, and bounded off-thread execution.

## Validation, refresh, and revocation

An access token is validated at startup, after reconnect-sensitive failures, and at least once per
hour while connected, as Twitch requires:

```text
GET https://id.twitch.tv/oauth2/validate
Accept: application/json
Authorization: OAuth <access-token>
```

Success is exactly `200` and must identify this application's exact client ID, one canonical positive
decimal user ID, one valid login, a positive expiry, and exactly `user:write:chat`. That user ID is the
only broadcaster identity accepted by chat delivery. `401`, an unexpected client ID, an invalid
identity, or a missing/different scope requires reconnection.

Refresh sends a URL-encoded public-client request with no secret:

```text
POST https://id.twitch.tv/oauth2/token
Accept: application/json
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token&refresh_token=<refresh-token>&client_id=<application-client-id>
```

Success follows the same strict token-grant rules as Device Code polling. Twitch refresh tokens
rotate, so the replacement access token and replacement refresh token are one atomic session update.
The application must durably protect the complete replacement before publishing it as connected or
using it for chat. It must never persist only one half of the pair. Twitch documents access tokens as
approximately four hours and refresh tokens as expiring after 30 days without use; neither lifetime
is treated as indefinite.

Disconnect revokes the current access token, then removes the protected local Twitch session:

```text
POST https://id.twitch.tv/oauth2/revoke
Accept: application/json
Content-Type: application/x-www-form-urlencoded

client_id=<application-client-id>&token=<access-token>
```

Revocation success is exactly `200`. Local disconnect remains fail-safe: protected credentials are
not kept merely because network revocation could not be confirmed. The application-owned ordering is
frozen in [`twitch-authentication-workflow.md`](twitch-authentication-workflow.md).

## Broadcaster-owned chat delivery

Chat is dependent on dps.report. A message may be scheduled only after the same upload job has a
validated dps.report permalink and the configured posting policy/template produces a valid message.
Delivery sends:

```text
POST https://api.twitch.tv/helix/chat/messages
Accept: application/json
Authorization: Bearer <access-token>
Client-Id: <application-client-id>
Content-Type: application/json

{
  "broadcaster_id": "<authenticated-user-id>",
  "sender_id": "<authenticated-user-id>",
  "message": "<rendered-message>"
}
```

`broadcaster_id` and `sender_id` are always the same ID returned by validation. There is no channel
name, channel ID, sender ID, or raw token option. `for_source_only` is deliberately omitted; Twitch's
documented user-token behavior therefore applies to shared-chat sessions.

Messages must be non-empty valid UTF-8, contain no ASCII controls, and contain at most 500 Unicode
code points. The client also caps their UTF-8 representation at 2,000 bytes before allocating the
request body.

Success is exactly `200` with exactly one data item. A sent item must provide a bounded message ID
and no drop reason. A rejected item must provide no message ID and one bounded visible drop-reason
code/message pair. `is_sent: false` is a successful Helix exchange but a failed delivery result; the
application presents the generic bounded reason without treating the message as posted.

## Failure, retry, and secret policy

Transport cancellation is cancelled. Connectivity, TLS, send/receive, and timeout failures are
retryable with bounded defaults. HTTP `408`, numeric bounded `Retry-After` on `429`, and `5xx` are
retryable. Validation or chat `401`, and refresh `400`/`401`, require reconnection. Other request,
authorization, protocol, or malformed-response failures are permanent for that operation.

The synchronous client performs no automatic refresh or retry. The implemented authentication
controller owns validation cadence, pre-expiry rotation, and polling/retry timing. The implemented
chat worker owns its separately bounded one-401 recovery and duplicate-risk policy through the
application-owned session-access boundary. Those rules are frozen in
[`twitch-chat-delivery.md`](twitch-chat-delivery.md).

Access tokens, refresh tokens, device codes, OAuth form documents, sensitive authorization headers,
and token response bodies are held in wipe-on-destruction storage wherever the HTTP boundary permits.
Errors never include a token, client request body, user message, user ID, login, response document,
raw server message, URL query, or exception detail.

## Deterministic coverage

Fake-transport tests cover client-ID validation, exact endpoints/forms/headers/timeouts, Device Code
success and pending responses, token rotations, scope and identity binding, revocation, same-ID chat
payloads, omission of `for_source_only`, sent and dropped messages, 500-code-point Unicode bounds,
malformed/bounded responses, every status class, transport classification, cancellation, and secret
redaction. Template and provider-worker tests additionally cover final Unicode bounds, one recovery,
drop normalization, safe retry classes, ambiguous-delivery suppression, and bounded dedupe. Default
tests never contact Twitch.
