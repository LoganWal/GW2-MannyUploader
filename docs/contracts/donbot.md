# DonBot upload contract

This contract defines version 1 of GW2 Manny Uploader's DonBot integration. It covers API-key
verification, server-authorized guild discovery, and one-shot TUS upload of a stable `.zevtc` file.

Verified 2026-08-20 against:

- the current DonBot [`UploadEndpoints.cs`](https://github.com/LoganWal/GW2-DonBot/blob/main/DonBot.Api/Endpoints/UploadEndpoints.cs);
- the live DonBot web configuration at <https://donbot.walmslo.com/config.js>; and
- the [TUS 1.0 protocol](https://tus.io/protocols/resumable-upload).

The live web application currently publishes `https://donbot-api.walmslo.com` as its API base. The
base remains ordinary configuration so self-hosted deployments can use the same integration. It must
be an absolute HTTPS URL of at most 2048 visible ASCII bytes, with a non-empty authority and no
credentials, query, fragment, backslash, or dot-path segment. Trailing slashes are normalized away
when endpoints are composed.

## Authentication and guild discovery

The Guild Wars 2 API key is a protected `DonBotGw2ApiKey` record. It never appears in ordinary JSON,
immutable snapshots, URLs, result details, or server error text. A usable key is 1 through 512 ASCII
letters, digits, or hyphens.

Verification sends:

```text
POST <api-base>/api/upload/gw2/guilds
Accept: application/json
Content-Type: application/json

{"apiKey":"<protected GW2 API key>"}
```

The secret JSON body is assembled and streamed from wipe-on-destruction storage. The key is not
duplicated into a request header for this endpoint. A successful `200` response is capped at 256 KiB
and must contain:

```json
{
  "accountName": "Player.1234",
  "guilds": [
    { "guildId": "123456789012345678", "guildName": "Raid Guild" }
  ]
}
```

Unknown fields are accepted for forward compatibility. Account and guild display names are non-empty
valid UTF-8 of at most 256 bytes with no ASCII controls. At most 256 guilds are accepted. Guild IDs
must be unique canonical positive decimal values within a signed 64-bit integer, matching the current
DonBot database/API boundary. The addon persists only the selected guild ID as ordinary settings;
verified account and list data are transient UI state.

DonBot performs the authoritative checks: the key must resolve through the Guild Wars 2 account API,
that account must be linked to a DonBot/Discord identity, and the returned list contains only DonBot
guilds in which that Discord user is a member. The addon never treats a locally entered guild ID as
authorization.

## TUS creation

Uploads require a non-empty selected guild ID and a protected key loaded immediately before the
attempt. Creation sends no body:

```text
POST <api-base>/api/upload/tus
Accept: application/json
X-GW2-API-Key: <protected key>
Tus-Resumable: 1.0.0
Upload-Length: <stable archive bytes>
Upload-Metadata: filename dXBsb2FkLnpldnRj,guildid <base64 guild ID>,wingman ZmFsc2U=
```

The implementation-owned filename decodes to `upload.zevtc`; it does not expose a local path or
Unicode filename. `wingman` always decodes to `false`. Direct GW2Wingman submission is the canonical
independent provider, so DonBot must not enqueue a second Wingman upload.

Success is exactly `201`. The response must contain exactly one `Tus-Resumable: 1.0.0` and one
`Location`. An optional `X-Log-Upload-Id` must be a unique positive signed-64-bit decimal value.
Response bodies are capped at 64 KiB.

## Upload location policy

TUS permits absolute and relative `Location` values, but replaying `X-GW2-API-Key` to an arbitrary
absolute URL would disclose the credential. The client therefore accepts only:

- a simple implementation-generated file ID;
- `<configured-base-path>/api/upload/tus/<file-id>` as a root-relative path; or
- the same path at the exact same HTTPS origin.

The file ID is 1 through 256 ASCII letters, digits, hyphens, or underscores. User information,
alternate origins, lookalike hosts, queries, fragments, backslashes, percent escapes, traversal, and
additional path segments are rejected before PATCH. Redirects remain disabled in the HTTP adapter.

## TUS PATCH and completion

The same stable file identity is opened only after creation succeeds. PATCH streams the original file
without building it in memory:

```text
PATCH <validated location>
X-GW2-API-Key: <same protected key>
Tus-Resumable: 1.0.0
Upload-Offset: 0
Content-Type: application/offset+octet-stream

<exact stable .zevtc bytes>
```

Success is exactly `204`. The response must contain exactly one `Tus-Resumable: 1.0.0` and one
decimal `Upload-Offset` equal to the original file size. The source size and last-write time are
verified before opening and after the final bytes are read.

## Failure and retry policy

Before TUS creation succeeds, cancellation is cancelled; transient transport errors, `408`, bounded
numeric `Retry-After` on `429`, and `5xx` are retryable; other local, protocol, authentication,
authorization, or request failures are permanent.

After creation succeeds, any PATCH transport error, non-`204` status, or invalid completion response
is permanent and described as unconfirmed. A server may have accepted the final bytes even when the
client did not receive the completion response. Automatically starting a new TUS upload could create
a duplicate DonBot record. Version 1 deliberately chooses no automatic retry after creation; resumable
HEAD/reconciliation can be added later only with persistent per-upload state and its own contract.

All errors use generic local text plus typed status/transport categories. They never include the API
key, URL, file path, account, guild name, response body, raw server message, or exception detail.

## Deterministic coverage

Fake-transport tests cover verification JSON and bounds, secret placement, all accepted location
forms, cross-origin and path-confusion rejection, exact metadata including `wingman=false`, streamed
file bytes, both TUS response handshakes, optional upload IDs, cancellation, local validation,
creation retry classification, and no-retry ambiguous PATCH failures. Default tests never contact
DonBot.
