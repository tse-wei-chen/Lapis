# ADR 0008: Scheduler execution and presentation stream boundary

- Status: Accepted
- Date: 2026-10-17

## Context

The decode worker now produces epoch-scoped `VideoFrameStream` objects and the timing layer can classify a frame as early, presentable, late, or stale. The next step must execute that policy without collapsing the architecture into a single decode/render thread.

A scheduler that directly calls D3D presentation would couple clock policy, waiting mechanics, graphics-device lifetime, and swap-chain behavior. It would also remove the explicit Render Thread boundary required for future GPU surfaces, device recovery, HDR, and presentation pacing.

Pause and seek add another constraint: a scheduler waiting for a future timestamp must wake immediately when the authoritative clock changes, rather than polling or sleeping through a discontinuity.

## Decision

1. `PlaybackClockSnapshot` carries `PlaybackEpoch`, media position, state, and a monotonic `revision` in one locked snapshot.
2. Timeline discontinuities use `PlaybackClock::rebase(epoch, position, now)` so epoch and media-time anchor change atomically.
3. Every successful clock mutation advances the revision and notifies waiters.
4. The clock exposes stop-aware revision waits: indefinite `wait_for_change` and timed `wait_for_change_for`.
5. `VideoSchedulerWorker` consumes one `VideoFrameStream` and produces a separate bounded `VideoPresentationStream`.
6. Scheduler execution never calls a renderer, graphics API, or presentation sink directly.
7. While the clock is paused or stopped, the scheduler waits for a clock revision change instead of polling.
8. For early frames on a running clock, the scheduler waits for either the computed timing timeout, a clock revision change, or stop cancellation, then reevaluates the same frame.
9. Each scheduling decision compares the frame epoch with the epoch from the same authoritative clock snapshot used for timing.
10. Stale and too-late frames are dropped before the presentation boundary and counted separately.
11. Released frames retain their original epoch/timestamp and also record the clock position and timing offset observed when released.
12. The presentation stream is bounded. A slow future Render Thread therefore applies backpressure rather than creating an unbounded ready-frame backlog.
13. Seek/open/graph-discontinuity orchestration must stop the old execution generation before mutating decoder/demux state and must create new epoch-scoped decode and presentation streams afterward.

## Consequences

- Render implementation can evolve independently to D3D11/D3D12, HDR, VRR, or device-loss recovery without moving timing policy into graphics code.
- Pause consumes no scheduler polling loop; resume and seek wake waiting scheduling work immediately.
- Epoch and position cannot be sampled from different timelines by the scheduler.
- A render consumer must still validate epoch before final presentation because a discontinuity can occur after a frame is released but before the Render Thread consumes it.
- Presentation pacing against actual display/vblank timing remains future work; `VideoPresentationStream` currently means "eligible to present now", not "already synchronized to a vblank".
