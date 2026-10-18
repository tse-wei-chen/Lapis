# ADR 0031: Video output binding crosses the C ABI as a separate presentation port

- Status: Accepted
- Date: 2026-10-18

## Context

ADR 0030 added thread-affine, acknowledged output-surface rebinding to the native video render worker and routed control through `PlaybackExecution`. The C ABI and managed runtime adapter still lacked a way for desktop hosts to bind/replace/detach a window. Playback transport and presentation-target ownership are distinct, so adding a HWND parameter to Open/Play or exposing the worker to managed code would break existing layering.

ABI v1 callers have no output-surface export. Accepting an old DLL using the v1 version check and calling the missing export later would fail at runtime, so this is an ABI v2 extension.

## Decision

1. Bump `LAPIS_MEDIA_RUNTIME_ABI_VERSION` and `NativeAbi.Version` from 1 to 2. The managed adapter fails fast if loaded against an ABI v1 DLL.
2. Add `lapis_video_output_surface` containing a fixed-width C enum and naturally aligned `uintptr_t` handle. `None` requires handle 0; `Win32Window` requires nonzero. This is a borrowed HWND, not a handle transferred to Lapis. No Windows header or D3D/DXGI type enters the ABI or application port.
3. Add `lapis_session_set_video_output_surface(session, descriptor, current_epoch, out_binding)`. A binding request checks the session's current epoch and does not advance it. `LAPIS_OUTPUT_SURFACE_APPLIED` reports render-thread acknowledgement; `DEFERRED` reports no active render generation and safe detachment of the old host target. Invalid/stale/unsupported/unavailable/faulted results remain distinct.
4. Use the existing ABI `command_mutex` so window changes serialize with Open/Play/Pause/Seek/Close and session disposal's caller-held lifetime rule. The binding command never holds the short-lived session state mutex while waiting on the render thread.
5. Save desired output binding on the logical native session, not the opened media pipeline. When Open replaces a pipeline, reapply the stored destination before making the new pipeline observable. A failed binding is not committed and the previous desired binding is restored to the worker.
6. Add a separate `IVideoOutputSurfaceBinding` application port. `NativePlaybackRuntime` implements it alongside `IPlaybackRuntime`; Application/Contracts do not reference P/Invoke, HWND-specific Windows assemblies, or the native adapter.
7. Marshal `NativeVideoOutputSurface` with natural `StructLayout.Sequential` alignment: enum int32, then native-sized unsigned pointer. On x64 the handle offset is 8 and struct size is 16; on x86 they are 4 and 8. Do not use Pack=1.
8. Expose `ClearVideoOutputSurfaceAsync` as managed convenience, implemented using the empty descriptor; a separate native clear export is unnecessary.
9. Validate descriptors and cancellation before lazy native session creation, then recheck cancellation under the adapter's serialization gate. Convert native results using the existing error mapping and an explicit output-unavailable code.
10. Cover native C struct layout/header, argument/epoch checks and detached-state binding, plus managed layout, mapping, session identity, cancellation, detach and failures with fake ABI tests.

## Lifetime semantics

A caller owns the HWND and must keep it alive until replacement/detach succeeds or the session is closed/destroyed. The ABI returns `APPLIED` only after render-thread acknowledgement that the previous window is no longer used. A `DEFERRED` success means no live rendering generation is holding the previous destination. A live presenter may still reject a non-empty HWND until a concrete D3D11/DXGI backend exists; no claim of on-screen video is made by this contract-only slice.

## Deferred

- Concrete Win32 D3D11/DXGI swap-chain presenter composition and real window rendering.
- Window validation, resize/minimize/DPI/fullscreen and device-loss recovery.
- Desktop host lifecycle ownership and thread dispatch.
- Host notifications/events and optional IPC presentation adapters.
