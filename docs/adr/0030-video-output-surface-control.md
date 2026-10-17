# ADR 0030: Video output-surface changes are render-thread control

- Status: Accepted
- Date: 2026-10-18

## Context

The public playback path now reaches the real native `PlaybackExecution`, but video presentation is still intentionally headless. Before adding a D3D11/DXGI presenter or exposing HWND binding through the stable C ABI, Lapis needs an output-surface lifetime contract that does not let control-plane threads mutate thread-affine graphics state directly.

Lapis already uses `VideoSurfaceKind` for decoded-frame backing storage (`SystemMemory`, `D3D11Texture`, `D3D12Resource`). That answers where a frame lives. The new output-surface contract answers where frames are presented, so it deliberately uses the separate `VideoOutputSurface*` vocabulary.

A naive `set_hwnd()` field is unsafe. The render worker can be blocked waiting for a scheduled frame or can already be inside `present()` while the UI destroys or replaces the window. If a surface mutation merely swaps an integer handle, the call can return while the presenter still owns a swap chain or other resources tied to the old window.

Output-surface binding is also different from a playback discontinuity. Rebinding a host window must not flush decode queues, advance `PlaybackEpoch`, seek media, or restart audio.

## Decision

1. Introduce a platform-neutral `VideoOutputSurfaceDescriptor` contract. The first concrete kind is `Win32Window`, represented only by an opaque `uintptr_t`; Windows headers and DXGI/D3D types do not enter core execution contracts.
2. `IVideoPresenter` gains `set_output_surface()`. It is invoked only by `VideoRenderWorker`, on the same render thread that owns `begin()`, `present()`, and `end()`.
3. `VideoOutputSurfaceApplyStatus::Ready` is a lifetime acknowledgement: after the presenter returns Ready, it must no longer access resources owned by the previously bound output surface. A failed apply must not retain resources owned by the rejected replacement.
4. `VideoRenderWorker::set_output_surface()` serializes public output-surface mutations. While a generation is Running, the call is synchronous and does not return `Applied` until the render thread has applied that exact revision.
5. Output-surface control has its own replaceable stop source. A binding request can wake a render worker blocked in the bounded presentation queue without stopping the playback generation. Only the render thread renews that source after observing the revision, preventing command-check-to-wait lost wakeups.
6. An output surface configured while the render generation is inactive is `Deferred`. No presenter can still be using the old surface after the previous generation has ended; the desired descriptor is retained and re-applied when the next generation starts.
7. The worker explicitly detaches a non-empty output surface on generation teardown after `end()`. The desired descriptor remains stored, but presenters may not retain host-window references while inactive.
8. Invalid descriptors are rejected before entering the render thread. `None` requires a zero handle; `Win32Window` requires a non-zero opaque handle.
9. A presenter that rejects or loses a requested output surface causes the render worker to enter `PresenterUnavailable`; opaque failures enter `Faulted`. Presenter exceptions during apply or teardown detach are contained inside the worker and translated to `Failed` rather than escaping the `noexcept` render loop.
10. The existing headless presenter remains source-compatible through the default interface implementation, which accepts `None` and rejects concrete output surfaces.
11. `PlaybackExecution::set_video_output_surface()` is the orchestration entry point. Its existing control mutex serializes output-surface rebinding against start, seek, close, and generation teardown; external adapters do not get direct worker access.
12. Output-surface rebinding does not advance playback epoch and does not touch audio, demux, decode, processing, scheduling, or the authoritative `PlaybackClock`.

## Consequences

- A future C ABI bind/clear output-surface call can delegate to `PlaybackExecution` instead of writing an HWND into shared state or reaching into the render worker.
- Returning from an active detach/replacement can become a reliable boundary for UI window destruction or ownership transfer.
- Decoded-frame backing surfaces and presentation targets remain distinct concepts as GPU decode is added later.
- D3D11/DXGI resources remain render-thread-affine and can be recreated without exposing graphics types upward.
- Output-surface changes can wake an otherwise idle render worker without polling and without tearing down the playback generation.
- Headless playback continues to work unchanged until a concrete presenter is composed.

## Deferred

- Public C ABI output-surface bind/clear functions.
- Managed output-surface adapter APIs.
- Concrete Win32 HWND + D3D11/DXGI swap-chain presentation.
- Resize/minimize/DPI/fullscreen policy.
- Device-loss recovery and surface recreation.
- HDR metadata, DXGI color-space selection, and presentation-mode policy.
