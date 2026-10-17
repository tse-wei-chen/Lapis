# ADR 0016: Audio render worker owns output pacing and drain lifecycle

- Status: Accepted
- Date: 2026-10-17

## Context

Audio decode now publishes bounded, epoch-tagged `AudioFrameStream` generations, and ADR 0015 introduced an OS-independent audio-output boundary. A real device path needs one more ownership decision before a WASAPI adapter is added: which component handles device backpressure, discontinuity flush, and the difference between decoder EOF and samples actually reaching the speaker.

Polling `try_submit()` would waste a core and would fight WASAPI's event-driven model. Treating input EOF as playback completion would also truncate the semantic clock because accepted samples may still be queued in the device.

## Decision

1. `AudioRenderWorker` is the sole consumer of an `AudioFrameStream` for one playback epoch and owns all calls into its `IAudioOutput` on the worker thread except the explicitly thread-safe position snapshot.
2. Output format is opened lazily from the first valid decoded frame. A failed open leaves the adapter closed.
3. Mid-generation decoded-format changes are rejected as `UnsupportedFormat` until a dedicated audio processor/resampler exists. The renderer never silently reinterprets samples.
4. `try_submit()` remains non-blocking. `Backpressure` must be followed by stop-aware `wait_until_writable()`; concrete adapters map this to native readiness events instead of busy polling.
5. Decoder/input EOF is not render completion. After the stream closes normally, the worker calls `wait_until_drained()` and enters `Drained` only after all accepted samples have left the device queue.
6. A seek/close-style stop interrupts either stream reads or device waits. Interrupted and failed generations call `reset()` before `close()` so buffered samples cannot leak across epochs.
7. Output-device loss maps to `OutputUnavailable`, distinct from malformed decoded data (`Faulted`) and unsupported format.
8. Submission statistics count only accepted frames/sample-frames. Backpressure waits and output failures are observable independently.
9. `AudioOutputPosition` remains an adapter-level presentation estimate. It does not become the global playback master clock in this change.

## Consequences

- The upcoming WASAPI adapter has an event-driven contract that matches Windows audio-client semantics instead of forcing a polling shim.
- Natural audio completion can be distinguished from decoder drain and from control-plane cancellation.
- Seek/epoch transitions have a deterministic device-flush boundary.
- Device recovery can be implemented without conflating device loss with decoder failure.
- `PlaybackExecution` still owns only the video generation today; integrating audio generation ownership and A/V clock discipline is deliberately the next layer.

## Deferred

- Concrete WASAPI shared/exclusive output.
- Device mix-format conversion, resampling, volume, and channel remapping.
- `PlaybackExecution` ownership of `AudioDecodeWorker` + `AudioRenderWorker`.
- Audio-master clock selection, latency compensation, underrun recovery, and drift correction.
