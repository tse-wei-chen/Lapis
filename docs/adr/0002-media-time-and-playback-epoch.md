# ADR 0002: Canonical MediaTime and PlaybackEpoch

- Status: Accepted
- Date: 2026-10-17

## Context

Blu-ray PTS, FFmpeg time bases, audio sample positions, presentation timestamps, and AI subtitle timestamps use different clocks and units. Seek and track switches also allow late asynchronous results from an older playback timeline to arrive after the user has moved elsewhere.

## Decision

- `MediaTime` is signed 64-bit nanoseconds inside the core.
- source-native timestamps may be retained for diagnostics, but adapters convert them before entering core scheduling contracts.
- each playback session owns a monotonically increasing `PlaybackEpoch`.
- open, seek, close, and future discontinuities advance the epoch.
- asynchronous payloads must carry `SessionId + Epoch`; consumers discard stale epochs.

## Consequences

Time semantics no longer depend on FFmpeg constants or `TimeSpan`, and stale post-seek frames/subtitles/AI results can be rejected deterministically rather than through timing guesses.
