# ADR 0015: Audio output adapter boundary

- Status: Proposed
- Date: 2026-10-17

## Decision

- Keep decoded `AudioFrame` and channel layout in native core contracts; no WASAPI or FFmpeg types leak into output policy.
- `IAudioOutput` represents one output-generation device attachment. Opening a format is explicit; unsupported formats must be converted by a separate audio processor/resampler, never silently reinterpreted.
- `try_submit` is non-blocking and returns explicit backpressure/device errors. An accepted frame's storage must remain valid until the device finishes with it; a rejected frame remains caller-owned.
- `reset` discards pending audio on seek/epoch discontinuity and advances the output clock revision. The device-reported presentation position is not the last submitted frame timestamp.
- Before accessing any plane, adapters validate sample width, layout, plane count, lengths, and nonzero sample rate using `valid_audio_buffer`.

## Deferred

- Actual WASAPI shared/exclusive adapter and device change recovery.
- Audio conversion/resampling to device mix format, volume, and channel mapping.
- Audio render worker and bounded audio queue integration.
- Clock discipline (latency estimation, underruns, drift, master-clock selection).

This ADR fixes the interface seam, not a claim that audio playback already works.
