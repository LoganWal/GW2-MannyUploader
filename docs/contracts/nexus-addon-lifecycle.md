# Nexus addon lifecycle contract

The native DLL exports the official `GetAddonDef` entry point and delegates process work to one
application runtime. Native Nexus and ImGui types stop at that entry adapter.

## Loading

1. Validate the API pointer and every native function used by the adapter.
2. Install the Nexus-provided ImGui context and allocator functions.
3. Resolve the game and addon directories into owned filesystem paths.
4. Construct the complete runtime before accepting render callbacks.
5. Register the main render callback, then the options callback.
6. Open the callback gate only after both registrations succeed.

Callbacks invoked synchronously during registration are safe no-ops. A partial registration failure
deregisters completed registrations in reverse order, shuts down the runtime, and returns to the
unloaded state.

## Callback boundary

The lifecycle gate admits callbacks only while running and counts every admitted callback. No native
callback may allow an exception to cross into Nexus. Exceptions are converted to a generic critical
log message that contains no path, setting, credential, HTTP document, or provider payload.

Rendering calls only application runtime methods. The runtime supplies immutable snapshots and queues
commands; it does not parse files, perform HTTP, or write settings inside the render callback.

## Unloading

1. Close the callback gate so newly delivered callbacks become no-ops.
2. Deregister options and main callbacks in reverse registration order.
3. Wait for every already-admitted callback to return.
4. Tell the runtime to cancel queues, stop acceptance, and join every owned worker.
5. Destroy runtime state and release the host pointer before returning from unload.

Unload is idempotent. No detached thread, queued callback, borrowed Nexus pointer, or runtime-owned
resource may survive its return. Nexus must not re-enter unload from inside one of this addon's render
callbacks.
