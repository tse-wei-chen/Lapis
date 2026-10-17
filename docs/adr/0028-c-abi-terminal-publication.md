# ADR 0028: C ABI snapshots are observational; terminal transport has one writer

- Status: Accepted
- Date: 2026-10-18

## Context

After public transport was delegated to `PlaybackExecution`, natural Ended/Faulted state became asynchronous. The first implementation allowed `lapis_session_get_snapshot` to latch that terminal state back into `lapis_session`. That made a read operation a second transport writer.

A control command can hold the per-session command mutex, release the short-lived state mutex while it waits for `PlaybackExecution`, and then publish its result. A concurrent snapshot could observe Ended and write `LAPIS_TRANSPORT_ENDED` during that window, after which the command could publish PAUSED/PLAYING from an older validation. The next snapshot would repair the visible state, but public transport could temporarily regress from a terminal state.

## Decision

1. `lapis_session_get_snapshot` is observational. It copies the published session fields and pipeline under the state mutex, releases that mutex, derives effective transport/position from `PlaybackExecutionSnapshot`, and returns them without mutating `lapis_session`.
2. Result-returning transport commands remain the only public transport writers under `command_mutex`.
3. Before Play, Pause, or Seek validates mutable transport state, it calls `refresh_terminal_transport` while holding `command_mutex`.
4. That refresh snapshots the active `PlaybackExecution` outside the state mutex and publishes only Ended/Faulted. It revalidates epoch, pipeline identity, and that the published transport is still active before committing the terminal state.
5. Close does not require a terminal refresh because it is valid as a teardown operation independently of natural completion.

## Consequences

- Snapshot reads cannot race a control command by writing terminal transport behind it.
- Natural completion is still visible immediately to snapshot callers, even before a later command latches it.
- The next mutating command sees terminal execution state before state validation and therefore cannot Pause/Resume/Seek a generation that has already drained or faulted.
- Long execution snapshots are not performed while holding the short-lived public state mutex.
- The command mutex remains the single serialization point for public transport mutation.
