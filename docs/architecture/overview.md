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
  - clock and scheduler
  - video / audio / subtitle hot paths
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

## Current implementation slice

The current vertical slice establishes a local `FileSource` backed by `IMediaByteStream`, feeds those bytes to FFmpeg through custom AVIO, demuxes and software-decodes one video stream, and converts the result into the Lapis `VideoFrame` surface abstraction. Blu-ray navigation, audio output, rendering, and AI remain deliberately outside this slice.
