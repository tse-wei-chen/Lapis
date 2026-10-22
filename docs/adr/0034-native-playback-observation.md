# ADR 0034: Native playback observations are reconciled by the Application actor

- Status: Accepted
- Date: 2026-10-18

## Context

The first Windows desktop host in ADR 0033 uses command-driven `PlaybackSnapshot` values. Sending Play changes the managed actor to Playing, but the native engine may advance its authoritative clock, finish the media naturally, or fault asynchronously. Without native observation, UI controls stay in the wrong state and position/duration cannot be reliably displayed. The C ABI v2 already exports `lapis_session_get_snapshot`; adding a new event export or bumping ABI merely to read existing data would be unnecessary.

## Decision

1. Add an optional `IPlaybackRuntimeObserver` port in Application, separate from the transport `IPlaybackRuntime`. It returns a `RuntimePlaybackObservation` with session identity, epoch, native position and transport state.
2. Add the blittable `NativePlaybackSnapshot` / `NativeTransportState` marshaling models to `Lapis.Runtime.Native.Interop`. Use the existing C ABI v2 `lapis_session_get_snapshot` export; keep session GUID byte identity and native layout intact.
3. The native adapter serializes observation with its existing handle-lifetime gate; it rejects unbound/wrong sessions, checks cancellation before crossing interop, and validates negative native positions or unknown native transport codes. Observation never creates a session or mutates a playback generation.
4. The Application actor starts an observation task **only** for runtimes that implement the port. Every 150 ms while Playing/Paused it reads the native snapshot off the UI thread, then uses a nonblocking `TryWrite` into its bounded command mailbox. Reads cannot stall the command actor or build an unbounded queue.
5. Only the actor's command-processing loop publishes `PlaybackSnapshot`. It accepts observations when the session and epoch match its current state and the transport is active. Mismatched/stale/negative observations are dropped. An old Playing observation may not roll a newly Paused actor back to Playing (or vice versa); terminal Ended/Faulted results are accepted.
6. Publish an explicit `PlayerError` when the native runtime reports Faulted, or when the observation API itself fails. Do not silently leave the UI stuck in Playing on persistent read failure.
7. Carry the already available `RuntimeOpenResult.Duration` into `PlaybackSnapshot.Duration` (nullable). Clear it on Open-start and Close to avoid reusing an old title's duration.
8. The desktop shell displays the native clock position and known duration, enables a duration-relative TrackBar, and seeks through the existing hosting port when dragging ends; it never invents UI-local playback time.
9. Cancel and join the observation task **before** destroying the native runtime/session, to avoid P/Invoke after handle disposal.
10. Add fake-ABI tests for snapshot structure layout, epoch/state/position mapping and invalid payloads; actor tests for live position, natural completion, native fault, seek epoch races, pause race and disposal ordering.

## Consequences

- Existing runtimes that do not implement the optional observation port remain supported and command-driven.
- Existing native C ABI v2 remains unchanged; this is a managed observation and UI slice.
- Bounded, cancellable observation uses at most one in-flight native read per session and one timer task. The actor remains the single writer of playback state.
- A future event-driven native adapter can implement the same observation port while preserving the actor's session/epoch guard.

## Amendment (2026-10-22): recoverable observations and terminal delivery

A snapshot read exception faults the current playback epoch, but must not
permanently terminate the session's observation task. Otherwise a successful
Close/Open after a transient device or interop failure would lose position and
end-of-media observation for all subsequent titles.

- Observation failures are placed in the bounded actor mailbox using
  cancellable asynchronous backpressure, and the observer awaits actor
  acknowledgement before polling again. While Faulted, polling remains idle;
  it resumes when a newly opened movie enters Playing/Paused.
- Routine nonterminal position updates stay best-effort `TryWrite` and may be
  dropped under load. Terminal Ended/Faulted observations instead use bounded,
  cancellable `WriteAsync` so a temporarily full queue cannot permanently
  suppress an end-of-media or fault transition.
- The serialized actor alone publishes state. Session and epoch checks still
  discard any observation or failure from a previous playback generation.
  Cancellation still joins the observer before disposing native handles.
- A real actor test injects a read exception, verifies Faulted, closes and
  reopens, then verifies the new epoch emits positions and Ended.

This updates Decision 4's *nonblocking best-effort queue* policy only for
terminal observations and failures. There is still at most one in-flight
native read and a fixed-capacity command mailbox; awaiting space happens on
the observation task, never on the UI or command actor.

## Deferred

- Frame-accurate progress and native push-based state-change notifications.
- Recoverable GPU device loss, detailed native error payloads and stream diagnostics.
- Slider preview, thumbnails, chapters, playlist and BDMV navigation.
