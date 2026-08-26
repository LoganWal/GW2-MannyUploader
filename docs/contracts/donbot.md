# DonBot upload contract

This contract defines GW2 Manny Uploader's DonBot integration. It covers API-key verification,
server-authorized guild discovery, permalink import when dps.report is enabled, one-shot TUS upload
when dps.report is disabled, and the completion stream used to retain the DonBot fight ID.

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

## dps.report permalink import

When the same job enables dps.report, DonBot waits for its successful result and sends:

```text
POST <api-base>/api/upload/gw2/url
Accept: application/json
Content-Type: application/json
X-GW2-API-Key: <protected key>

{"url":"https://dps.report/<permalink>","guildId":"<selected guild ID>"}
```

The permalink must have the exact `https://dps.report/` prefix and no credentials, query, fragment,
backslash, control, or non-ASCII byte. The selected guild and key use the same validation and
authorization boundary as a direct upload. The server stores the selected guild, deduplicates by
authorized identity, guild, and canonical URL, and disables its own Wingman submission.

New work returns `202`; an idempotent existing result returns `200`. Both responses are capped at 64
KiB and must contain:

```json
{
  "uploadId": 42,
  "fightLogId": null,
  "status": "pending",
  "duplicate": false
}
```

`uploadId` is a positive signed-64-bit integer. `duplicate` is required. `status` is `pending` with a
null fight ID, or `complete` with a positive signed-64-bit fight ID. Pending work reuses the anonymous
completion stream below. The import POST is idempotent, so a transient transport error, `408`, `429`,
or `5xx` remains retryable. Invalid input, authentication, authorization, or response data is a
permanent failure.

If dps.report fails or is cancelled, DonBot is skipped or cancelled without making a request. A
dps.report retry re-arms skipped DonBot work. A DonBot retry after dps.report success imports the same
retained permalink.

## Direct fallback TUS creation

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
Unicode filename. `wingman` always decodes to `false`, so DonBot must not enqueue a second Wingman
upload.

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

When creation returned a numeric upload ID, successful PATCH completion is followed by an anonymous
bounded event-stream request:

```text
GET <api-base>/api/upload/stream/<upload-id>
Accept: text/event-stream
```

Each `data:` line must contain a bounded JSON object with a stage. A `failed` stage is a permanent
processing failure. A `complete` stage ends processing and may include one positive signed-64-bit
`fightLogId`. That ID is retained with the upload job and used only to compose
`https://donbot.walmslo.com/logs/aggregate?ids=<comma-separated-fight-ids>` for the current visible
rows. The aggregate link never uses TUS upload IDs.

Failure to reach the optional progress endpoint does not reinterpret an already-confirmed TUS upload
as failed and leaves the fight ID unavailable. Cancellation remains cancellation. A malformed or
explicitly failed event stream is a permanent DonBot result because automatically creating a second
upload could duplicate the accepted log.

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

Fake-transport tests cover verification JSON and bounds, permalink import and idempotent responses,
secret placement, all accepted location
forms, cross-origin and path-confusion rejection, exact metadata including `wingman=false`, streamed
file bytes, both TUS response handshakes, optional upload IDs, cancellation, local validation,
creation retry classification, and no-retry ambiguous PATCH failures. Default tests never contact
DonBot.
