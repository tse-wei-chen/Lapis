# Architecture overview

Lapis is designed as a media platform rather than a UI wrapped around one decoder stack.

```text
UI / future frontends
        |
        v
Application control plane
  - command mailbox
  - playback session
  - immutable GraphPlan
  - capability decisions
        |
        | stable adapter boundary
        v
Native MediaRuntime
  - source / demux / decode adapters
  - clock and scheduler
  - video / audio / subtitle hot paths
        |
        +---- media taps ----> AIHost (future, non-blocking)
```

## Invariants

1. UI does not decode or render media.
2. Application code does not manipulate packet/frame/GPU hot-path objects.
3. Third-party libraries are adapters at the edge.
4. `MediaTime` is the canonical core time model.
5. Every discontinuity that can make async work stale advances `PlaybackEpoch`.
6. Playback commands have one logical owner and are serialized.
7. Runtime graph changes are represented as new immutable plans, not arbitrary live mutation.
8. AI observes playback and never sits in the blocking playback path.
9. Extension decisions are capability-based, not concrete-player-type checks.
10. Native ABI types are plain C-compatible values; C++ implementation details never cross the boundary.

## Next implementation slice

After this foundation, the next vertical slice should be a minimal file source with FFmpeg isolated behind a native adapter: open a local file, demux, software-decode one video stream, and expose diagnostics without yet adding Blu-ray navigation or AI.
