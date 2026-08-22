# Nexus addon lifecycle contract

The native DLL exports the official `GetAddonDef` entry point and delegates process work to one
application runtime. Native Nexus and ImGui types stop at that entry adapter.

## Loading

1. Validate the API pointer and every native function used by the adapter.
2. Install the Nexus-provided ImGui context and allocator functions.
3. Resolve the game and addon directories into owned filesystem paths.
4. Construct the complete runtime before accepting callbacks.
5. Register the main render callback, options callback, and configurable window input bind.
6. Attempt the embedded normal, idle-grey, and Twitch-purple icon textures and quick-access shortcut,
   then open the callback gate.

Callbacks invoked synchronously during registration are safe no-ops. A partial registration failure
deregisters completed required registrations in reverse order, shuts down the runtime, and returns to
the unloaded state. Quick access is optional: a texture/shortcut failure emits a warning while main
rendering, options, the input bind, and the complete upload runtime remain active. Icon creation uses
Nexus's synchronous memory API; no asynchronous texture callback may retain code or data from the
addon DLL.

## Callback boundary

The lifecycle gate admits render and input-bind callbacks only while running and counts every
admitted callback. Input-bind release notifications are ignored; a press toggles the main window. No
native callback may allow an exception to cross into Nexus. Exceptions are converted to a generic
critical log message that contains no path, setting, credential, HTTP document, or provider payload.

Callbacks call only application runtime methods. The runtime supplies immutable snapshots and queues
commands; it does not parse files, perform HTTP, or write settings inside a Nexus callback. Window
visibility uses a dedicated value command so toggling cannot overwrite unrelated provider settings.
The main callback may refresh the shortcut from an immutable status snapshot. The tooltip begins
with `View MannyUploader list`, adds one line per enabled destination and the selected DonBot guild,
and uses Twitch purple in preference to the normal upload tint. Nexus has no in-place shortcut update,
so a changed status is applied as one balanced remove/add pair with owned tooltip storage.

## Unloading

1. Close the callback gate so newly delivered callbacks become no-ops.
2. Remove the quick-access shortcut, deregister the input bind, then deregister options and main
   rendering in reverse registration order.
3. Wait for every already-admitted callback to return.
4. Tell the runtime to cancel queues, stop acceptance, and join every owned worker.
5. Destroy runtime state and release the host pointer before returning from unload.

Unload is idempotent. No detached thread, queued callback, borrowed Nexus pointer, or runtime-owned
resource may survive its return. Nexus must not re-enter unload from inside one of this addon's
callbacks. Nexus owns the cached decoded texture; the addon retains no texture pointer after
registration.
