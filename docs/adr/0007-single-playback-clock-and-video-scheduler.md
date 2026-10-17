# ADR 0007: Single authoritative playback clock and video scheduling policy

- Status: Accepted
- Date: 2026-10-17

## Context

Once decoded frames cross worker boundaries, playback needs one answer to the question "where is playback now?". Allowing video, audio, subtitles, UI, and AI to maintain independent current-time values would create drift and make pause/seek behavior inconsistent.

The video path also needs a deterministic rule for deciding whether a frame is early, ready, late-but-usable, or too late to present.

## Decision

1. `PlaybackClock` is the single authoritative media-time clock inside `MediaRuntime`.
2. The clock maps an explicit monotonic time sample to canonical `MediaTime`; it does not read wall-clock time internally.
3. `SteadyClockSource` is the initial production monotonic source. Future audio-hardware clock support will discipline/rebase this same playback clock rather than creating a second authoritative current-time model.
4. Clock state is explicit: `Stopped`, `Running`, or `Paused`.
5. Pause freezes the media position. Resume creates a new monotonic anchor at the frozen position. Seek replaces the media/monotonic anchor without implicitly changing the current clock state.
6. If a supplied monotonic sample is older than the active anchor, playback position does not move backward.
7. Position arithmetic saturates at the maximum representable `MediaTime` rather than overflowing.
8. `VideoScheduler` is a pure decision policy. It does not sleep, render, mutate the clock, or own a thread.
9. Scheduler decisions evaluate epoch staleness first, then classify timing as `Wait`, `Present`, or `Drop`.
10. A stale epoch is always dropped. A sufficiently early current-epoch frame waits; a slightly early/late frame may present; a frame beyond the configured late threshold is dropped.
11. Early tolerance and late-drop threshold are configuration/tuning values, not ABI constants.

## Consequences

- Video, subtitles, UI state, and later AI alignment can all project against one canonical clock.
- The future renderer can change waiting/presentation mechanics without changing timing policy.
- The future audio path can become the clock master by feeding observations into this clock model rather than inventing a parallel timeline.
- Playback-rate changes are intentionally not implemented yet; the current clock advances at 1.0x. Rate support should extend the clock mapping explicitly rather than multiplying timestamps ad hoc in consumers.
- Scheduler thresholds will require empirical tuning once real rendering, hardware decode, and display timing are available.
