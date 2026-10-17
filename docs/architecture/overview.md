# Architecture overview

Lapis is designed as a media platform rather than a UI wrapped around one decoder stack.

```text
UI / future frontends
        |
        v
Application control plane
  - command mailbox
  - playback session
  - playback policy / intent
  - capability-aware product decisions
        |
        | stable C ABI / adapter boundary
        v
Native MediaRuntime
  - immutable GraphPlan / GraphBuilder
  - source / demux / decode adapters
  - bounded decode execution streams / workers
  - authoritative playback clock
  - video scheduler worker -> bounded presentation stream
  - video render worker -> IVideoPresenter adapter
  - future audio / subtitle hot paths
        |
        +---- media taps ----> AIHost (future, non-blocking)
```

## Invariants

1. UI does not decode or render media.
2. Application code does not manipulate packet/frame/GPU hot-path objects or concrete graph nodes.
3. Third-party libraries are adapters at the edge.
4. `MediaTime` is the canonical core time model.
5. Every discontinuity that can make async work stale advances `PlaybackEpoch`.
6. Playback commands have one logical owner and are serialized.
7. There is one executable graph model, owned by native `MediaRuntime/Core`.
8. Runtime graph changes are represented as new immutable plans, not arbitrary live mutation.
9. AI observes playback and never sits in the blocking playback path.
10. Extension decisions are capability-based, not concrete-player-type checks.
11. Native ABI types are plain C-compatible values; C++ implementation details never cross the boundary.
12. Potentially blocking media I/O never holds the authoritative session-state mutex; work is committed only if its epoch is still current.
13. Every cross-thread media queue is bounded; no playback stage may grow an unbounded backlog.
14. Every cross-thread timed sample carries `PlaybackEpoch` plus canonical `MediaTime`.
15. Each execution epoch owns closeable stream/queue generations; a queue is never reopened for a later timeline.
16. `PlaybackClock` is the single authoritative playback-time model; its snapshot binds clock state, `PlaybackEpoch`, `MediaTime`, and revision atomically.
17. Video scheduling policy is separate from waiting/rendering mechanics and always rejects stale epochs before timing decisions.
18. Clock mutations notify scheduler waiters; paused/stopped playback must not be implemented as polling.
19. Video Scheduler and Render Worker remain separate stages connected by a bounded `VideoPresentationStream`.
20. A frame is revalidated by epoch at every asynchronous boundary, including immediately before the presenter adapter.
21. Graphics APIs are presenter adapters; decode/scheduler/core code never depends on D3D/DXGI types.
22. Presenter lifecycle callbacks are Render Worker thread-affine, and stale-only streams do not initialize graphics state.

## Current implementation slice

The current vertical slice establishes a local `FileSource` backed by `IMediaByteStream`, feeds those bytes to FFmpeg through custom AVIO, demuxes and software-decodes video, and converts frames into the Lapis `VideoFrame` surface abstraction. The execution layer provides stop-aware bounded queues and a `VideoDecodeWorker` that publishes frames into an epoch-scoped `VideoFrameStream`. `PlaybackClock` owns epoch, media position, state, and revision in one authoritative snapshot, while `VideoSchedulerWorker` waits efficiently against that clock, drops stale/late frames, and releases eligible frames into a second bounded `VideoPresentationStream`. `VideoRenderWorker` now consumes that stream, performs a final clock/epoch gate, and calls a thread-affine `IVideoPresenter` adapter with explicit device-loss/failure results. Blu-ray navigation, audio output and hardware-clock discipline, actual D3D11/DXGI presentation, hardware decode, subtitles, and AI remain deliberately outside this slice.
