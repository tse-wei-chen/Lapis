# ADR 0029: Managed native runtime is an adapter below Application

- Status: Accepted
- Date: 2026-10-18

## Context

`Lapis.Application` already defines `IPlaybackRuntime` as the port used by the playback-session actor, and the native C ABI now delegates transport to the real `PlaybackExecution`. There was still no managed implementation of that port, so the managed control plane could not actually invoke the native runtime.

Putting P/Invoke declarations in `Lapis.Application` would invert the architecture: the application layer would own ABI layout, native-library naming, pointer lifetime, UTF-8 allocation, and platform loading concerns. It would also make future IPC or sandboxed runtime adapters harder to introduce.

The native C ABI allows concurrent result-returning calls, but a managed adapter additionally owns lazy session creation and destruction. Those lifetime transitions must not race disposal or bind one adapter instance to multiple logical playback sessions.

## Decision

1. Add a separate `Lapis.Runtime.Native` project that references `Lapis.Application` and `Lapis.Contracts`. Neither Application nor Contracts references the adapter.
2. `NativePlaybackRuntime` implements `IPlaybackRuntime`. It is the composition-edge adapter from managed playback commands to ABI v1.
3. Raw `[LibraryImport]` declarations and ABI-layout structs live only under `Lapis.Runtime.Native.Interop`.
4. The adapter checks `lapis_runtime_get_abi_version()` before creating a runtime and rejects incompatible libraries before any session exists.
5. One `NativePlaybackRuntime` instance binds to exactly one `PlaybackSessionId`. The native session is created lazily on the first command and then reused for subsequent Open/Play/Pause/Seek/Close calls.
6. Runtime and session native handles have deterministic managed leases. Disposal destroys the session before the runtime and is idempotent.
7. UTF-8 locator memory is allocated only for the duration of `lapis_session_open`, matching the C ABI borrowed-pointer contract.
8. Native result codes are translated at the adapter boundary: invalid state/stale epoch become `InvalidOperationException`, unsupported/ABI mismatch become `NotSupportedException`, out-of-memory remains `OutOfMemoryException`, and opaque internal failures become `NativeMediaRuntimeException`.
9. Commands check cancellation before crossing into native code. Once a synchronous ABI call starts it is not interrupted, because ABI v1 has no cancellation handle yet. The Application playback actor already executes runtime calls away from the UI thread.
10. The adapter serializes its own handle/session lifetime with a private gate even though the native ABI supports concurrent result operations. This protects direct adapter use and disposal without changing the higher-level playback actor's single-command ownership.
11. Adapter tests use an injected internal native-API seam and therefore validate mapping, lifetime, session binding, result translation, and cancellation without loading a platform DLL.

## Consequences

- The managed playback actor can now be composed with the real native media runtime without leaking P/Invoke into Application.
- A future IPC/out-of-process runtime can implement the same `IPlaybackRuntime` port beside the in-process native adapter.
- ABI-layout changes are isolated to one project and guarded by an explicit version check.
- Native library deployment/loading remains a host concern; this slice does not choose a desktop UI framework or bundle the DLL into an application package.
- The next presentation slice can add a surface-binding port/adapter without coupling that concern to transport commands or Application contracts prematurely.

## Deferred

- Native snapshot/event propagation into managed state beyond command results.
- ABI cancellation/interrupt handles for long blocking native operations.
- Native-library probing/bundling policy for desktop distribution.
- Stable video surface binding and concrete D3D11/DXGI presentation.
- Desktop UI host composition and dependency injection.
