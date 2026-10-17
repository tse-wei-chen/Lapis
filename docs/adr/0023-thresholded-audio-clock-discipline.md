# ADR 0023: Audio hardware observations are filtered before clock correction

- Status: Accepted
- Date: 2026-10-18

## Context

`WasapiSharedAudioOutput` can now publish a portable hardware-clock snapshot containing canonical media time, the Lapis monotonic time at which the device position was sampled, a discontinuity revision, and an explicit source-quality marker.

That snapshot must not be wired directly to `PlaybackClock::rebase()` on every refresh. `PlaybackClock` revisions wake `VideoSchedulerWorker` so that pending frame timing is reevaluated after meaningful clock changes. Rebasing for every device observation would turn normal hardware sampling noise into scheduler wakeups and make the device adapter responsible for synchronization policy.

The synchronization layer also needs to reject observations that are not hardware-correlated, were sampled too long ago, are implausibly in the future, or arrive while the logical playback clock is intentionally paused/stopped.

## Decision

1. Add a platform-independent `AudioClockDiscipline` policy in the execution layer. It depends only on `AudioOutputPosition`, `PlaybackClockSnapshot`, and `MonotonicTime`; it has no WASAPI, COM, FFmpeg, or worker ownership.
2. The policy never mutates `PlaybackClock`. It returns one of three decisions: `Ignore`, `Maintain`, or `Rebase`. The eventual playback orchestrator owns any clock side effect.
3. Only `AudioOutputPositionSource::HardwareClock` may drive discipline. Estimated/unavailable positions remain diagnostics/fallback data until an explicit fallback policy exists.
4. Discipline is evaluated only while `PlaybackClock` is Running. Paused/stopped playback does not extrapolate the device observation into a correction.
5. A valid observation is extrapolated from its `sampled_at` time to the caller-supplied `now` at normal playback rate. The resulting audio media position is compared with the already-sampled `PlaybackClockSnapshot::position`.
6. Observations older than a configurable freshness limit are ignored. Small future correlation errors are tolerated, but larger future timestamps are rejected instead of extrapolated backwards.
7. A configurable symmetric deadband suppresses corrections for small positive or negative drift. The default policy uses a 30 ms rebase threshold, 200 ms maximum observation age, and 2 ms future-timestamp tolerance. These are policy defaults, not platform invariants, and remain configurable.
8. A `Rebase` decision carries the extrapolated audio media position, signed drift, and output revision. It does not imply that the caller must immediately apply it; future orchestration may add hysteresis, rate limiting, or a different master-clock policy without changing device adapters.
9. Media-time and drift arithmetic saturate at representable limits rather than overflowing.

## Consequences

- Normal hardware sampling does not automatically increment `PlaybackClock` revision or wake video scheduling waits.
- Device-clock acquisition and synchronization policy stay independently replaceable.
- Unit tests can cover freshness, correlation tolerance, drift direction, deadband boundaries, and overflow behavior without a physical audio endpoint.
- The next `PlaybackExecution` integration can coordinate audio worker lifecycle and then apply discipline decisions at a deliberate control point.

## Deferred

- Applying `Rebase` decisions inside `PlaybackExecution`.
- Hysteresis/rate limiting beyond the deadband if real-device measurements show it is needed.
- Frequency/rate correction or resampler compensation for long-term clock drift.
- Audio-master versus external/master-clock policy.
- Device-loss recovery and output revision continuity policy across endpoint replacement.
