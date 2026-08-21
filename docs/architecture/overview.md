# Architecture overview

GW2 Manny Uploader uses a layered ports-and-adapters architecture. The core workflow is testable
without Guild Wars 2, Nexus, ImGui, the filesystem, or public HTTP services.

## Dependency direction

```text
Nexus / ImGui / Win32 adapters
             |
             v
      application layer
             |
             v
        domain layer

HTTP / filesystem / config adapters
             |
             +---- implement application-owned ports
```

The dependency direction always points inward:

- `domain` uses only the C++ standard library and defines job state and policy.
- `application` coordinates use cases and owns the ports required from external systems.
- `providers`, `filesystem`, `config`, and `platform` implement those ports.
- `addon` composes the application and owns Nexus registration and shutdown.
- `ui` renders immutable snapshots and submits commands.

Nexus, ImGui, HTTP, JSON, ZIP, and Win32 types do not cross into the domain layer.

The outer lifecycle boundary is also project-owned and portable. It gates and counts native callback
entries, contains every exception, reverses partial registration, and blocks unload until admitted
callbacks drain. The exact current Nexus/ImGui ABI pin is recorded in
[`ADR 0006`](decisions/0006-pin-current-nexus-api-and-imgui-abi.md); ordered hot-unload behavior is
frozen in [`docs/contracts/nexus-addon-lifecycle.md`](../contracts/nexus-addon-lifecycle.md).

## Runtime flow

```text
watch candidate
      |
      v
stability + dedupe
      |
      v
pending upload job
      |
      v
asynchronous EVTC metadata
      |
      +----> dps.report ----> Twitch chat
      +----> GW2Wingman
      +----> DonBot
```

dps.report, GW2Wingman, and DonBot are independent. Twitch is the only dependent provider and may
start only after dps.report has produced a valid permalink.

## State ownership

Each log receives a stable `UploadJobId`. One coordinator owns all mutable `UploadJob` instances.
Workers return typed results through bounded queues; they never mutate jobs. The UI receives immutable
snapshots, so rendering cannot race provider completion.

The implemented domain foundation enforces:

- non-zero job IDs and non-empty paths;
- enabled providers begin waiting and disabled providers remain terminal;
- explicit provider transition rules;
- retry timestamps only on scheduled retries;
- attempt counting when work becomes active;
- dps.report completion requires a non-empty permalink;
- Twitch cannot activate without a successful dps.report result;
- encounter metadata is assigned once.

The application coordinator is the sole mutable job owner. It:

- allocates monotonically increasing stable job IDs;
- dispatches dps.report, GW2Wingman, and DonBot independently;
- holds Twitch in `Waiting` until dps.report supplies a valid permalink;
- correlates every provider result by job ID and provider ID;
- schedules retries against an injected monotonic clock;
- evicts only settled jobs when the bounded history is full;
- publishes deep-copy snapshots for consumers such as the UI; and
- cancels provider queues and unsettled job states exactly once during shutdown.

Provider implementations receive value-type upload requests through `IUploadProvider`; they never
receive a mutable job reference. Time enters the application through `IClock`, keeping timestamps and
retry tests deterministic.

## Metadata ingestion boundary

A stable, unique file creates a pending `UploadJob` before parsing starts. Enabled providers remain in
`Waiting`, and no upload request is emitted yet. `LogIngestionCoordinator` submits a value-type
`MetadataParseRequest` through the application-owned `ILogMetadataParser` port. The request contains
the stable job ID and complete file identity, so asynchronous results never rely on container indexes
or paths alone.

The parser adapter returns a typed result to the coordinator owner:

- success assigns encounter metadata once and starts the independent upload providers;
- a parse failure marks every enabled provider failed with a visible diagnostic;
- parser cancellation marks the pending provider states cancelled; and
- failure to enqueue parsing creates a retained failed job rather than silently dropping the file.

`MetadataParserWorker` implements the asynchronous port with one owned `std::jthread` and bounded FIFO
request/result queues. It contains reader exceptions, preserves job IDs, applies output backpressure,
and requests cooperative cancellation before joining during destruction. The coordinator owner drains
completed results; the worker never receives mutable job state.

## Minimal EVTC payload decoder

The archive-independent decoder accepts a read-only byte span containing a revision-1 EVTC payload.
It extracts only the header boss/species ID and the account name associated with the
`CBTS_POINTOFVIEW` source address. It does not retain agent tables, skill tables, or combat events.

The implementation reads little-endian fields explicitly and never casts file bytes to native C++
structures. Count-derived offsets are bounded before iteration; partial tables and events are
rejected. Agent and skill counts have explicit caps, combat-event scanning is capped, and cancellation
is checked while scanning. The matched account's NUL-delimited structure and UTF-8 encoding are
validated before producing `EncounterMetadata`.

The exact supported subset and its public arcdps sources are documented in
[`docs/contracts/evtc-metadata-subset.md`](../contracts/evtc-metadata-subset.md). ZIP entry selection,
CRC/decompression validation, and compressed/uncompressed size limits remain the responsibility of
the `.zevtc` archive adapter rather than the binary decoder.

## `.zevtc` archive adapter

The archive adapter uses pinned miniz through a custom offset-read callback over
`std::ifstream(std::filesystem::path)`. This keeps platform paths in the standard filesystem type and
avoids narrow-character archive-open APIs. It requires exactly one case-insensitive `.evtc` entry,
ignores unrelated entries within a count cap, and rejects encrypted, unsupported, empty, duplicate, or
missing payload entries.

Archive bytes, entry bytes, filename bytes, central-directory entry count, and compression ratio are
bounded before payload allocation. Extraction proceeds iteratively in 64 KiB chunks, checks the stop
token between chunks, and requires final CRC/size validation. The complete behavior and typed error
mapping are documented in [`docs/contracts/zevtc-archive.md`](../contracts/zevtc-archive.md); the
dependency choice is recorded in
[`ADR 0002`](decisions/0002-miniz-for-zevtc-extraction.md).

## Log discovery policy

Filesystem adapters are responsible for producing canonical path, size, and last-write observations.
The standard-library-only discovery policy then:

- accepts `.zevtc` case-insensitively and rejects other extensions;
- requires at least two consecutive matching size/write-time observations;
- tracks interleaved candidates independently;
- resets a candidate's match count whenever its size or write time changes;
- emits a `LogFileIdentity` and releases its pending state once stable; and
- suppresses exact path/size/write-time identities through a bounded oldest-first dedupe history.

The policy performs no filesystem reads and never sleeps. Scheduling observations, canonicalizing
paths, handling missing files, and selecting native-notification or polling mechanisms remain adapter
responsibilities. Repeated notifications after a stable emission are safe: stability may emit again,
but the deduplicator suppresses the unchanged identity.

The first concrete candidate source is a synchronous standard-filesystem poller. It is scheduled on
the application owner thread, never from a render callback. Missing roots are a normal state so an
arcdps directory created after startup is discovered later. Complete snapshots emit removals;
incomplete traversal or metadata reads retain prior paths and emit diagnostics without manufacturing
removals. Symlinks are not followed, candidate counts are bounded, and a stop token is checked during
enumeration.

`ApplicationPump` owns the discovery pipeline and performs bounded ticks. It drains completed
metadata results first, then polls, forgets confirmed removals, observes candidates, and submits newly
stable identities. The exact scheduling and failure contract is documented in
[`docs/contracts/log-polling-pipeline.md`](../contracts/log-polling-pipeline.md).

## Ordinary settings adapter

The configuration domain defines a version-1 aggregate independently of JSON and filesystem types.
Pure validation covers schema compatibility, path and numeric limits, provider URLs and identifiers,
Twitch dependencies, and the message-template grammar. Protected credentials are absent from every
ordinary-settings type and are rejected if they appear as unknown JSON keys.

`SettingsStore` is a synchronous adapter intended for one owner outside render callbacks. It caps the
document before parsing, uses Glaze with UTF-8 validation and strict unknown-key handling, preserves
defaults for missing nested version-1 fields, and requires an explicit top-level schema version.
Saving validates first, flushes a sibling temporary file, and atomically replaces the destination. A
valid previous primary is preserved in an independently atomic `.bak` update; corrupt input cannot
replace that last-known-good backup. Loading uses defaults only when neither file exists and reports
backup recovery rather than hiding primary corruption. The complete schema and recovery rules are in
[`docs/contracts/settings-schema-v1.md`](../contracts/settings-schema-v1.md); the dependency choice is
recorded in [`ADR 0003`](decisions/0003-glaze-for-json-settings.md).

## Protected credential adapter

Protected credentials cross the application boundary through `ISecretStore` as one of three stable
identifiers and a move-only opaque `SecretValue`. The interface supports only load, atomic replace,
and idempotent erase. Callers cannot choose filenames, and error values contain a credential ID,
category, safe static text, and an optional numeric system code rather than secret material.

`ProtectedFileSecretStore` wraps each value in a versioned, bounded envelope with its stable ID,
payload length, and CRC-32 before passing it to `ISecretProtector`. This independently detects corrupt
or wrong-record plaintext returned by a platform adapter. The protected bytes are written through the
same durable atomic-file primitive as settings, but secret records intentionally have no backup. Both
the primary and an interrupted-write temporary are removed on erase.

The production protector uses user-scoped, prompt-free Windows DPAPI and fixed application entropy.
It explicitly omits machine scope and wipes Windows-owned plaintext before release. Construction
detects Wine through its exported version symbol and fails as `UnsupportedEnvironment` before a
credential can be stored; non-Windows builds expose the same unsupported result. There is no
plaintext or embedded-key fallback. The complete behavior is in
[`docs/contracts/protected-credentials.md`](../contracts/protected-credentials.md), and the platform
decision is recorded in
[`ADR 0004`](decisions/0004-native-dpapi-and-fail-closed-wine-secrets.md).

## HTTP transport adapter

Provider code depends on a typed `IHttpClient` port containing only standard C++ values. Requests own
an absolute URL, ordered redaction-aware headers, bounded timeouts and response limits, and an optional
single-use exact-length pull body. Production validation permits HTTPS only; an explicit test policy
permits plaintext loopback URLs and cannot be enabled through user settings.

The Windows adapter uses pinned static libcurl with Schannel. Each call owns one easy handle and
streams upload data through exception-contained callbacks. Cancellation is checked before connection,
during upload reads, and through transfer progress. Response headers and body bytes are capped before
insertion, HTTP status codes remain successful transport results, and redirects are never followed
automatically. Errors contain typed categories and optional numeric backend codes, never URLs, query
strings, body data, paths, or header values.

The complete boundary is in [`docs/contracts/http-transport.md`](../contracts/http-transport.md), and
the WinHTTP/libcurl comparison, pinned build, and Wine probe are recorded in
[`ADR 0005`](decisions/0005-static-libcurl-schannel-http-transport.md).

## dps.report provider adapter

`DpsReportClient` is a synchronous provider adapter intended to run inside a bounded provider worker,
never on a Nexus callback. It depends only on `IHttpClient`, owns each request body for the duration of
one call, and returns typed success, retry, permanent-failure, or cancellation data. The fixed primary
endpoint and policy remain inside the provider; neither is ordinary user configuration.

The adapter composes exact-length pull sources for stable files, wipe-on-destruction secret memory,
and multipart framing. It therefore streams the original `.zevtc` without constructing the complete
upload in RAM. File size and last-write time are checked before opening and again before the final
bytes are accepted, preventing a discovered identity from silently becoming a different upload.

Successful JSON is parsed forward-compatibly but must contain a trusted HTTPS dps.report permalink and
complete bounded encounter data. Optional server warnings are reduced to generic diagnostics, and a
rotated user token is returned separately as a move-only secret for durable application-owned
persistence. Provider errors never contain the token, source path, request URL, response document, or
raw server message. HTTP and transport failures are classified by the rules in
[`docs/contracts/dps-report.md`](../contracts/dps-report.md).

`DpsReportProviderWorker` connects that synchronous adapter to `IUploadProvider` by owning the shared
`AsyncUploadWorker`, which provides one joined thread and bounded FIFO input/output queues. The
dps.report wrapper loads the optional protected token immediately before each attempt and persists any
replacement before making success visible. Anonymous uploads remain available when protected storage
is unavailable; newly issued tokens are then discarded securely with a generic warning rather than
persisted through an unsafe fallback.

Provider results carry a retry duration rather than a time point. The application pump drains a
bounded number per tick with round-robin provider fairness, and the coordinator validates each result
by stable job/provider ID before turning a delay into a monotonic deadline. This keeps jobs, time, and
retry transitions owned by the application thread while HTTP and protected-record I/O remain outside
Nexus callbacks. Queue,
credential, result, and shutdown behavior is frozen in
[`docs/contracts/dps-report-provider-worker.md`](../contracts/dps-report-provider-worker.md).

## GW2Wingman compatibility adapter

`WingmanClient` streams the same stable `.zevtc` identity with its POV account, decimal archive size,
and boss/species trigger ID to the existing raw-EVTC compatibility bridge. It strictly validates
inputs and the bridge's small JSON response, treats `409` as duplicate success, and classifies bounded
retries without exposing paths, accounts, URLs, or server text.

The current public GW2Wingman API documents only Elite Insights processed upload or import of an
already processed external link; it does not expose a raw-EVTC endpoint. Bundling Elite Insights and
its .NET runtime would violate this addon's streamlined native scope, while importing dps.report would
make Wingman dependent on another provider. The raw bridge is therefore isolated as an explicit,
replaceable compatibility risk rather than presented as an official API.

`WingmanProviderWorker` adds only Wingman-specific request/result rules and owns the same
`AsyncUploadWorker` used by dps.report. Both providers consequently share FIFO bounds, output
backpressure, exception containment, cooperative cancellation, and joined shutdown. The exact wire
and worker behavior is frozen in [`docs/contracts/wingman.md`](../contracts/wingman.md) and
[`docs/contracts/async-provider-workers.md`](../contracts/async-provider-workers.md).

## DonBot provider adapter

`DonBotClient` implements two current API operations: protected-key verification with authorized guild
discovery, and a two-request TUS upload. The creation metadata fixes the remote filename, supplies the
selected decimal guild ID, and always sets `wingman=false` so the independent direct Wingman provider
remains canonical. A TUS `Location` is accepted only beneath the configured creation path at the exact
same HTTPS origin before the same key can be sent in PATCH.

The client requires exact `201` creation and `204` completion handshakes, including protocol version
and final upload offset. Once creation succeeds, an ambiguous PATCH failure is not automatically
retried because starting a new upload could create a duplicate record. The complete wire and retry
rules are frozen in [`docs/contracts/donbot.md`](../contracts/donbot.md).

`DonBotProviderWorker` copies the ordinary API base and guild selection into each accepted request,
while loading the protected API key only when that attempt reaches the worker. Options changes
therefore affect new work without retargeting queued work, and credentials never enter coordinator
requests. See
[`docs/contracts/donbot-provider-worker.md`](../contracts/donbot-provider-worker.md).

DonBot options use a separate application-owned command path. `DonBotVerificationWorker` receives a
move-only key through `IDonBotVerifier` and performs account/guild verification away from Nexus
callbacks. `DonBotConfigurationController` correlates the result, owns transient account/guild state,
and publishes secret-free immutable snapshots. A newly verified endpoint is first persisted with
DonBot disabled and no selection, then the protected key is replaced, and only then does verified
identity become visible. Startup re-verifies the saved key; revoked selections are durably disabled.
Guild selection and disconnect are also write-through, with disconnect disabling ordinary settings
before protected-key erasure. The exact state and failure ordering is frozen in
[`docs/contracts/donbot-configuration-workflow.md`](../contracts/donbot-configuration-workflow.md).

## Twitch broadcaster client

`TwitchClient` is the synchronous external-service boundary for a public Twitch application. Addon
composition injects its production client ID; neither settings nor protected storage contains a
client secret. The client requests only `user:write:chat` and implements Device Code start/poll,
token validation, public-client refresh, revocation, and Helix Send Chat Message as typed operations
over `IHttpClient`.

Device codes, access tokens, and rotating refresh tokens remain move-only secrets. OAuth form bodies,
sensitive authorization headers, parsed token strings, and token response buffers are overwritten on
normal release. Validation binds a session to the exact application client ID, sole required scope,
and one canonical authenticated user ID. Chat uses that same ID for both broadcaster and sender, so
no separately configurable channel or sender can redirect a credential.

This boundary deliberately contains no polling loop, clock, retry loop, persistence, or job mutation.
`TwitchAuthenticationWorker` places each primitive behind one bounded joined thread and returns
move-only secrets with its typed result when policy may retry them. The worker owns no deadlines or
session state.

`TwitchAuthenticationController` owns public workflow policy on the application thread. It schedules
Device Code polling from Twitch's interval, validates saved and connected sessions, refreshes before
known expiry, permits one reconnect recovery path, and publishes only secret-free
login/code/URI/expiry snapshots. `TwitchSessionOwner` is the one serialized credential owner shared
by that controller and chat delivery. Revisioned exclusive transactions prevent scheduled auth,
chat-side `401` recovery, disconnect, and shutdown from publishing or persisting competing token
generations.
The complete token pair, expiry, identity, login, and sole granted scope are encoded into one bounded
versioned `TwitchOAuthSession` payload. Initial grants are validated before storage; rotated grants are
atomically stored before further validation, connected publication, or chat use. Disconnect first
durably disables Twitch, then attempts revocation off-thread, and erases the local session regardless
of the remote outcome.

`TwitchMessageTemplate` compiles the six-field version-1 grammar used by both settings validation and
delivery. `TwitchProviderWorker` captures that template and its result policy per accepted same-job
dps.report result, then renders and sends away from application and render callbacks. A narrow
`ITwitchDeliverySessionAccess` lease boundary keeps protected-session acquisition and one controlled
401 recovery with the authentication owner instead of letting the chat worker read secret storage.
Confirmed message IDs and normalized drop states return to the coordinator as typed receipts.

The same synchronous delivery policy is factored into `TwitchChatDelivery`. Encounter posting keeps
its bounded job/permalink ledger in `TwitchProviderWorker`, while explicit options tests use a
separate `ITwitchTestMessenger` request/result port and `TwitchTestMessageWorker`. A test request
contains only a correlation ID; the worker builds fixed ID-suffixed text and never fabricates a log,
upload job, encounter, or dps.report result. Both paths therefore share broadcaster-session leasing,
one-`401` recovery, drop normalization, retry safety, ambiguity suppression, and redaction without
coupling the options action to the upload coordinator.

Automatic chat retries are deliberately narrower than the synchronous client's general transport
classification. Only failures known to precede delivery, plus explicit `429`, may retry. Ambiguous
attempts are sealed in a bounded job/permalink ledger, preventing a timeout or malformed success from
posting twice. The exact wire behavior is frozen in
[`docs/contracts/twitch.md`](../contracts/twitch.md); session lifecycle is frozen in
[`docs/contracts/twitch-authentication-workflow.md`](../contracts/twitch-authentication-workflow.md);
template, lease, receipt, recovery, and duplicate behavior is frozen in
[`docs/contracts/twitch-chat-delivery.md`](../contracts/twitch-chat-delivery.md).

## Application configuration ownership

`ConfigurationService` owns the `ISettingsStore` and the available `ISecretStore`; adapters do not
outlive their application owner. Construction loads ordinary settings once. Failure to obtain a valid
primary, backup, or default configuration is fatal, while unavailable protected storage becomes an
explicit `Available`, `UnsupportedEnvironment`, or `InitializationFailed` capability in the
configuration snapshot. This lets Wine continue with providers that need no credentials without
pretending it can persist tokens securely.

Snapshots deep-copy validated ordinary settings, load/recovery state, protected-storage capability,
revision, and shutdown state. They deliberately contain neither credential values nor credential
presence. Ordinary changes are write-through: the service updates and revises its snapshot only after
the settings adapter reports a durable save. Explicit credential load, replace, and erase operations
route through the protected port without caching their values in application state. All public
operations are internally serialized because a delivery worker may rotate Twitch credentials while
the Nexus application thread reads or writes ordinary settings.

The service owns no threads. Its idempotent shutdown marks the final snapshot and rejects every later
settings or credential operation before an adapter can be touched. The complete boundary is in
[`docs/contracts/configuration-service.md`](../contracts/configuration-service.md).

## Nexus options boundary

Nexus options are composed through `NexusOptionsController`, not by exposing configuration or
provider objects to the render callback. The render side reads one deep-copy snapshot, maps it through
the pure `NexusOptionsModel`, and submits a move-owned command into a bounded FIFO. Submission may
validate and queue values in memory but performs no settings I/O, protected-record access, provider
dispatch, OAuth polling, or HTTP.

The application owner drains a bounded number of commands outside rendering and is the only caller of
the DonBot and Twitch configuration workflows. General, dps.report, Wingman, and Twitch message-policy
changes use an ordinary-options payload that cannot mutate workflow-owned DonBot or Twitch enablement.
Dedicated commands enforce verified DonBot endpoint/guild state and a connected broadcaster-owned
Twitch session before those destinations can be enabled.

An additional command queues one explicit Twitch test message only while connected and while no test
is in flight. The application controller correlates its result and publishes secret-free sending,
sent, error, normalized delivery, and ambiguity state. It never retries the test automatically; a
later button press is a new, uniquely identified user action.

The published composite snapshot remains credential-free. Candidate DonBot keys exist only in
move-only queued commands and worker requests; Twitch tokens remain in the session owner. A queued
key is wiped if command acceptance stops before dispatch. Status labels and control availability are
derived in a standard-library-only view model so behavior is tested without Nexus or ImGui. The full
boundary, queue policy, and shutdown responsibilities are frozen in
[`docs/contracts/nexus-options.md`](../contracts/nexus-options.md).

The production Windows adapter composes settings, protected-storage capability, Schannel HTTP,
provider clients/workers, authentication workflows, EVTC polling/parsing, and coordinators before it
opens the Nexus callback gate. One background application-owner thread drains commands, updates
provider configuration, polls the log directory, advances jobs, and publishes UI-ready deep copies.
The ImGui callbacks never perform filesystem traversal, persistence, HTTP, parsing, OAuth polling, or
worker shutdown.
The concrete ownership and scheduling rules are frozen in
[`docs/contracts/nexus-runtime-composition.md`](../contracts/nexus-runtime-composition.md).

## Shutdown

Cancellation originates at addon unload and flows through `std::stop_token`. Shutdown order is:

1. Close callback admission and deregister Nexus render resources.
2. Wait for already-admitted render callbacks to return.
3. Stop accepting UI/configuration commands and file candidates.
4. Cancel queued/in-flight metadata parsing and provider work.
5. Shut down authentication/session/configuration owners.
6. Join every worker and the background application-owner thread.
7. Destroy application state and release the Nexus API pointer.

Detached threads are prohibited.

## Testing

Tests mirror production modules. Domain and coordinator rules use fast unit tests with an injected
clock and fake provider ports. The EVTC decoder uses generated byte fixtures, including every truncated
prefix of a valid payload. Archive tests construct minimal ZIP byte fixtures for entry-selection,
resource-limit, truncation, and CRC cases; no copyrighted logs are required. Worker tests use
condition variables and stop callbacks rather than sleeps to make queue saturation and cancellation
deterministic. Protected-store tests use a deterministic fake protector for every file and envelope
rule; Windows CI adds a real DPAPI round trip and tamper test, while the cross-built suite under Wine
proves that the adapter fails closed. Configuration-service tests use owned fake ports to verify
startup, write-through snapshot commits, secret routing and redaction, capability reporting, and
post-shutdown isolation. The HTTP port has portable validation tests plus deterministic Windows
loopback tests for streaming, limits, redirect refusal, cancellation, and adapter lifetime; public
HTTPS probing is opt-in only. Provider clients use fake transports and deterministic fake-server
contract tests. The dps.report, GW2Wingman, and DonBot wrappers use condition-variable fakes to
exercise the shared worker's queue, backpressure, retry, exception, and shutdown behavior; dps.report
and DonBot additionally cover credential ordering. Twitch adds binary-session corruption fixtures,
primitive-worker backpressure, fake-clock controller tests for polling, validation, rotation,
persistence, disconnect, and shutdown, plus adversarial session-owner tests for stale leases,
recovery/controller exclusion, identity mismatch, and shutdown during recovery. Nexus checks remain
a small integration layer, not a substitute
for automated behavior coverage.

Every behavior change includes its test in the same change. External services are never contacted by
the default test suite.
