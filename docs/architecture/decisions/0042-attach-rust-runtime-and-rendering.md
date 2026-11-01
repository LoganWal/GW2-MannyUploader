# ADR 0042: Attach the Rust runtime and Nexus renderer

- Status: Accepted
- Date: 2026-10-16

## Decision

The Rust DLL constructs native application dependencies and starts one owned ApplicationRuntimeThread
before registering Nexus callbacks. Callbacks receive RuntimeHandle and adapter-owned presentation
state. Unload closes admission, reverses resource registration, drains callbacks, then cancels and
joins the application owner and every worker before clearing borrowed ImGui state.

The previous panic hook stays outside the construction unwind boundary until registration succeeds.
Failed construction restores it without calling set_hook from an unwinding destructor. Diagnostics
remain static and exclude panic payloads. Partial registration rollback shuts down the constructed
runtime through the same joined release path.

The renderer uses the pinned ImGui 1.80 ABI through a frame-scoped wrapper. Window, table, combo,
identifier, color, and text-wrap scopes balance their native stack operations during unwinding.
Unavailable actions render inert labels. Text input uses fixed initialized UTF-8 buffers with an
extra terminator byte. Credential buffers are wiped on transfer, verification, and destruction.
Ordinary drafts survive unrelated snapshots and later edits survive acknowledgment of an earlier
save. Visibility uses narrow commands and an immediate local value until the owner publishes them.

Options expose destination toggles, discovery settings, DonBot verification/guild/Discord workflows,
and broadcaster-owned Twitch connection, authorization, posting policy, template, and test messages.
The packaged Twitch application identity requires no developer setup field. Authorization renders
only the fixed code mask and a browser visibility warning. Verified DonBot replaces the key form
with server controls and disconnect. Its enable control is beside the server selector.

Recent logs use the existing pure models, stable job IDs, retained DonBot selection, and bounded
commands. The table omits raw log paths and shows only enabled destination columns. Copy buttons
write validated links through the host's ImGui clipboard callback. Browser/folder opening, replay,
retry, and aggregate delivery remain application-owner commands. Provider status and error text
wrap inside their table columns.

Quick-access status updates use owned tooltip storage and a balanced remove/add pair. The shortcut
mutex serializes the last admitted update with unload removal, and removal closes further updates.
This prevents an admitted callback from registering a shortcut after teardown has removed it.

## Validation and remaining gates

Native lifecycle fixtures use fresh temporary paths and disabled upload destinations. They cover
construction failure, hook restoration, registration rollback, optional resource failure, live
shortcut updates, post-removal isolation, input visibility, and joined owner shutdown. The real-DLL
smoke host likewise creates fresh isolated settings before its hot-load/render/unload cycles.

Renderer tests drive an actual ImGui mouse click and verify that settings persist only on an owner
tick. Tests also cover bounded UTF-8 input, secret transfer, draft preservation, save acknowledgment,
authorization masking, and completed rows without raw paths. Synthetic draw-data exports were
rasterized and inspected for the options page and recent-log table. Failing-before-fix regressions
cover candidate-key retention, verified credential-form replacement, and shortcut teardown closure.

The C++ addon remains the shipping artifact. This integration does not complete active-provider-HTTP
real-DLL unload coverage, Wine execution, native/Wine browser and clipboard checks, hosted native OS
gates, packaging, symbols, or release cutover. Those remain required migration work.
