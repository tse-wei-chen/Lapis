# ADR 0006: Epoch-scoped bounded execution streams

- Status: Accepted
- Date: 2026-10-17

## Context

The first FFmpeg slice could synchronously decode a video frame, but real playback needs demux/decode/scheduler/render stages to run independently. Unbounded queues would turn slow consumers into unbounded memory growth, while reusing one queue across seeks would allow stale frames from an earlier timeline to mix with the current one.

Playback discontinuities already advance `PlaybackEpoch`, so the execution layer needs to carry that timeline identity with every cross-thread sample.

## Decision

1. Every cross-thread media queue in `MediaRuntime` is bounded.
2. Blocking queue operations accept `std::stop_token` so shutdown can wake producers or consumers that are waiting for capacity/data.
3. Cross-thread timed samples use `TimedPayload<T>` and carry both `PlaybackEpoch` and canonical `MediaTime`.
4. A decode run creates a new `VideoFrameStream` with its own bounded queue. Streams are never reopened or reused for a later epoch.
5. When a decode run drains, stops, or faults, its stream is closed. Consumers may drain already-enqueued items and then observe `Closed`.
6. Starting a later epoch creates a different stream object. Old consumers can therefore only observe old-epoch samples or closure; they cannot consume frames from the new timeline.
7. `VideoDecodeWorker` owns only execution mechanics. FFmpeg types remain inside the FFmpeg adapter and do not leak into execution/core contracts.
8. Seek/open/graph-replacement orchestration will stop the old producer before mutating the underlying demux/decoder state and will publish a new epoch-scoped stream afterward.

## Consequences

- Backpressure is explicit instead of relying on process memory.
- Scheduler/render code can block efficiently without polling and can terminate cleanly when a stream closes.
- Epoch checks remain valid even if an old consumer or callback wakes late.
- Queue capacity becomes a deliberate tuning/policy value rather than an accidental implementation detail.
- A blocked FFmpeg read is not yet interruptible by `std::stop_token`; an FFmpeg interrupt/cancellation bridge remains a later requirement before network/Blu-ray sources are considered fully cancellable.
- End-of-stream and decode failure are still coarse at the decoder boundary because the current decoder returns `std::nullopt` for both. The worker exposes a `Faulted` state for thrown failures, but richer decode error/event propagation remains future work.
