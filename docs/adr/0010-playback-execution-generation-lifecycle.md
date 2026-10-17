# ADR 0010: Playback execution owns generation lifecycle

- Status: Accepted
- Date: 2026-10-17

## Context

Decode, scheduling, and rendering are now independent workers connected by bounded queues. That isolation prevents one stage from owning the entire media hot path, but it creates a lifecycle requirement: discontinuities must stop the generation in the correct direction before demux/decoder state is mutated.

If callers individually stop workers, it is easy to deadlock. A decoder may be blocked pushing to a full decode queue while the scheduler is waiting on a paused clock, and the scheduler may itself be blocked behind the render stage. The stop order is therefore part of the playback architecture, not a UI concern.

## Decision

1. `PlaybackExecution` is the single owner of `VideoDecodeWorker`, `VideoSchedulerWorker`, `VideoRenderWorker`, and the authoritative `PlaybackClock`.
2. Callers cannot receive or manipulate the internal stream generations through `PlaybackExecution`.
3. Generation startup order is Decode -> Scheduler -> Render; queue streams are passed downstream immediately and remain bounded.
4. Generation shutdown order is Render -> Scheduler -> Decode. Stop tokens wake any stage blocked on a downstream bounded queue or clock wait.
5. `start`, `seek`, and `close` accept a `PlaybackEpoch` and reject non-advancing epochs before touching media or worker state.
6. `pause` only pauses the authoritative clock. Workers remain alive and bounded queues naturally apply backpressure.
7. `resume` resumes the same generation and epoch.
8. A running seek first pauses the clock, then joins the old generation downstream-to-upstream, performs the media seek, atomically rebases clock epoch/position, rebuilds the generation while the clock is paused, then resumes only if the pre-seek state was Running.
9. A paused seek rebuilds workers but keeps the clock paused. This permits bounded predecode without allowing presentation until resume.
10. A stopped seek updates media position and clock epoch without starting workers.
11. Close makes the clock inactive, joins the old generation, then resets the clock to the new epoch and zero position.
12. Worker startup failures transition execution to `Faulted`; media seek failure also invalidates the old timeline and faults the execution.
13. `PlaybackExecutionSnapshot` exposes logical/effective execution state plus clock, worker states, scheduling stats, and render stats for future diagnostics and C ABI mapping.
14. If all active workers have drained, the snapshot reports `Ended` even though the internal transport owner has not yet been wired to publish a C ABI transport transition.

## Consequences

- Seek/open/close orchestration no longer depends on every caller remembering queue topology or thread order.
- A paused pipeline can intentionally backpressure all the way to decode without deadlocking seek or close.
- Presenter objects remain owned by the render worker and survive generation rebuilds inside the same `PlaybackExecution` instance.
- The public C ABI can later delegate play/pause/seek/close to one execution owner rather than coordinating individual worker types.
- Audio/subtitle workers can be added to the same lifecycle owner, with their stop order made explicit before they become part of the ABI.
- FFmpeg blocking-I/O cancellation is still a separate limitation: if a decoder thread is blocked inside source I/O rather than a Lapis queue, a future AVIO interrupt bridge is still required.
