# ADR 0022: Audio hardware clock snapshots stay portable

- Status: Accepted
- Date: 2026-10-18

## Context

The shared-mode WASAPI output now owns device submission and Pause/Resume on the render thread. Its existing `AudioOutputPosition` was a queue-padding estimate: submitted media time minus current endpoint padding. That is useful for diagnostics, but it is not a hardware clock and is not strong enough to become the reference for long-running A/V synchronization.

WASAPI exposes `IAudioClock::GetPosition`, which returns the stream-relative device position together with the QPC timestamp at which that position was sampled. The device-position unit is intentionally opaque and must be interpreted with `IAudioClock::GetFrequency`. `IAudioClient::Stop` freezes that position, `Start` resumes it, and `Reset` returns it to zero.

The future playback synchronizer must consume audio timing without depending on COM, QPC units, `IAudioClock`, or Windows headers. It also needs the clock sample's monotonic capture time so it can reason about sample age and extrapolation in the same time domain already used by `PlaybackClock`.

## Decision

1. `AudioOutputPosition` is the portable output-clock snapshot. It contains a media-timeline position, a `MonotonicTime sampled_at`, a generation/discontinuity revision, and an `AudioOutputPositionSource`.
2. A snapshot marked `HardwareClock` is suitable as an input to future clock discipline. `Unavailable` is explicit; consumers must not infer clock quality from a non-zero position.
3. WASAPI acquires `IAudioClock` and its `GetFrequency` value during `open()`. Failure to obtain the stream clock makes the output open fail rather than silently degrading the synchronization contract.
4. The first successfully released sample prefix establishes the mapping from WASAPI stream position zero to the Lapis media timeline. The origin is `frame.timestamp + first_sample_frame / sample_rate`.
5. Each render-thread refresh calls `IAudioClock::GetPosition`. The returned device position is converted through the reported device frequency, never by assuming it is expressed in sample frames.
6. WASAPI's QPC correlation is bridged into the Lapis monotonic domain by capturing a current QPC value beside `SteadyClockSource::now()` and applying the QPC delta. Raw QPC values never cross the adapter boundary.
7. `position()` never calls COM. The render thread publishes a cached snapshot and readers consume it through a small sequence lock, keeping the fields coherent without imposing COM apartment affinity on control/synchronization threads.
8. `Reset` clears the media origin, marks the clock unavailable, and advances the position revision. The next accepted prefix establishes a new origin. Pause/Resume preserve the origin and revision.
9. Queue-padding estimation is not used as a substitute hardware clock in this slice. If a future platform lacks a hardware-correlated clock, that fallback policy must be explicit rather than silently changing clock quality.

## Consequences

- Platform-specific clock types stay inside the output adapter.
- A future A/V synchronizer can compare an audio media position against `PlaybackClock` using a common monotonic correlation time.
- Pause semantics and hardware-clock semantics align: Stop freezes the stream clock without creating a discontinuity; Reset creates a new clock revision.
- The output's position snapshot is coherent even when read concurrently with render-thread refreshes.
- The product execution path is still video-only. This ADR does not yet make audio the master clock or wire audio workers into `PlaybackExecution`.

## Deferred

- Audio-master versus external/master-clock policy.
- Drift estimation, filtering, correction thresholds, and video scheduling against audio.
- `PlaybackExecution` ownership of Audio Decode -> Process -> Render.
- Device-loss recovery and clock re-acquisition.
- `IAudioClient3`, exclusive/bitstream output, and endpoint hot-switching.
