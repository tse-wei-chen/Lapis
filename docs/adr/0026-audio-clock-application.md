# ADR 0026: Audio clock corrections are sampled and compare-applied

- Status: Accepted
- Date: 2026-10-18

## Context

`PlaybackExecution` now owns optional audio generations and can expose a portable `AudioOutputPosition` from the active render path. `AudioClockDiscipline` can classify such a hardware observation as Ignore, Maintain, or Rebase, but deliberately does not mutate `PlaybackClock`.

Applying a Rebase recommendation introduces two independent concurrency problems. First, normal device observations must not become playback-clock revision cadence: each `PlaybackClock` revision wakes video scheduler timing waits. Second, Pause/Resume/Seek can change clock state, epoch, or revision after a synchronization worker samples the clock but before it attempts to apply a correction. An unconditional late `rebase()` could therefore move a paused clock or carry a hardware observation across a discontinuity.

The device and logical clock also transition in an intentional order. Pause freezes the audio endpoint before pausing `PlaybackClock`; Resume makes `PlaybackClock` Running before resuming the endpoint. A synchronization loop must not treat the frozen hardware position in either transition window as a valid master-clock observation.

## Decision

1. Add a generation-scoped `AudioClockSyncWorker` in the execution layer. It owns sampling cadence and the side effect of applying accepted `AudioClockDiscipline` decisions; platform adapters and the discipline policy remain unchanged.
2. The default sampling interval is 20 ms and is configurable through `PlaybackExecutionConfig`. Sampling frequency is independent of WASAPI event frequency and does not itself mutate `PlaybackClock`.
3. While `PlaybackClock` is Running, the worker reads an `AudioOutputPosition`, evaluates it against a snapshot taken for the same monotonic instant, and applies only `Rebase` decisions. Ignore and Maintain remain observation-only.
4. While `PlaybackClock` is Paused or Stopped, the worker does not poll. It blocks on `PlaybackClock::wait_for_change` and is awakened by a clock revision or generation stop.
5. `PlaybackClock` gains `try_rebase(expected_snapshot, position, now)`. The mutation succeeds only if state, epoch, and revision still match the snapshot that produced the correction. A concurrent Pause/Resume/Seek therefore wins cleanly instead of being overwritten by stale synchronization work.
6. Before attempting a Rebase, the synchronization worker re-reads the audio source revision. The hardware source must still be `HardwareClock` and carry the same output revision as the discipline decision. Output reset/device-loss discontinuities therefore reject cached recommendations.
7. `PlaybackExecution` supplies the synchronization source through the current `AudioRenderWorker`, but reports an unavailable observation unless that render worker is `Running`. A frozen Paused renderer cannot steer the logical clock during the Resume ordering window, and a drained/faulted renderer is not a master source.
8. The synchronization worker starts only after the generation's audio render worker starts, and stops/joins before the renderer/output is stopped or destroyed. Its position reader may therefore be non-owning without extending device lifetime.
9. Synchronization failure participates in `PlaybackExecution` worker-failure accounting. Snapshot diagnostics expose worker state plus evaluation, ignore/maintain, recommendation, applied, rejected, last-drift, and output-revision counters.
10. A successful clock correction keeps the same `PlaybackEpoch`; it is timing discipline inside one continuity domain, not a media discontinuity. It increments only `PlaybackClock` revision so waiting video schedulers reevaluate timing.

## Consequences

- Hardware audio becomes the first concrete timing source capable of disciplining the single authoritative `PlaybackClock` without creating a second product clock.
- Small drift inside the discipline deadband produces no clock revision and no video-scheduler wakeup.
- Pause/Resume/Seek races cannot apply stale asynchronous corrections after the logical clock has changed.
- Paused playback consumes no periodic synchronization wakeups.
- The sampling/application worker remains portable and can be tested with a synthetic hardware clock without WASAPI or a physical endpoint.
- Future master-clock policies can replace the reader/configuration or synchronization worker without changing FFmpeg, WASAPI, or decoded media contracts.

## Deferred

- Long-term frequency/rate correction or resampler compensation instead of discrete rebases.
- Additional hysteresis/rate limiting if real-device measurements show repeated threshold crossings.
- External/video master-clock policies and user-selectable synchronization strategies.
- Device-loss recovery, endpoint hot-switching, and continuity rules across a replacement endpoint.
- Tuning synchronization defaults against real Blu-ray content, high refresh-rate video, Bluetooth endpoints, and passthrough/exclusive output modes.
