# ADR 0009: Render Worker and presenter adapter boundary

- Status: Accepted
- Date: 2026-10-17

## Context

The scheduler execution stage now emits a bounded `VideoPresentationStream` containing frames that were eligible for presentation when released. A final asynchronous boundary still exists between scheduler release and the graphics API. A seek or other discontinuity can happen in that gap, so presentation cannot trust scheduler-time validation alone.

Binding the scheduler directly to D3D would also collapse timing, graphics-device lifetime, device-loss handling, and render-thread affinity into one component.

## Decision

1. `VideoRenderWorker` is the only execution component that calls an `IVideoPresenter`.
2. `IVideoPresenter` is a third-party-independent adapter contract receiving Lapis `VideoFrame` surfaces and a plain `VideoPresentContext`.
3. `begin`, `present`, and `end` are invoked exclusively from the Render Worker thread. A presenter may create or bind thread-affine graphics state there.
4. The presenter object persists across stream generations; `begin`/`end` describe a render-generation session, not necessarily graphics-device lifetime.
5. Presenter initialization is lazy. A stream that becomes stale before its first valid frame does not invoke `begin` at all.
6. Before every `present`, the Render Worker samples the authoritative `PlaybackClock` and validates the frame epoch again.
7. A frame whose epoch differs from the stream epoch or current clock epoch is dropped before the presenter boundary.
8. A frame reaching the Render Worker while the authoritative clock is paused or stopped is dropped rather than advancing the visible image after transport has become inactive.
9. Presenter results are explicit: `Presented`, `UnsupportedSurface`, `DeviceLost`, or `Failed`.
10. Unsupported surfaces and device loss end the current render generation as `PresenterUnavailable`; other presenter failures end it as `Faulted`.
11. `end` is called exactly once for every successful `begin`, including presenter failure after initialization.
12. Discontinuity orchestration must still stop/join the old Render Worker before rebasing the clock or replacing media state. Final epoch validation is defense in depth, not a substitute for lifecycle ordering.

## Consequences

- D3D11, D3D12, software/test, and future presentation backends can be implemented without changing decode or scheduling policy.
- Device-loss recovery can replace/reinitialize the presenter at the render boundary rather than leaking DXGI semantics into scheduler/core code.
- Stale frames can be rejected even if they were valid when the scheduler released them.
- The current presenter call is synchronous and may not be interrupted while inside backend code. Real DXGI presentation must therefore avoid unbounded waits and later integrate frame-latency/vblank primitives deliberately.
- Window ownership, resize, HDR metadata, swap-chain selection, and actual D3D implementation remain separate follow-up work.
