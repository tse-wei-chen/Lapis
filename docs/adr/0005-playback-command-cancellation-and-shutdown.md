# ADR 0005: Playback command cancellation and session shutdown

Status: Accepted

## Context

`PlaybackSession` is the single managed actor that serializes transport mutations. UI operations can carry short-lived cancellation tokens, while native open/seek operations may already have changed epoch and transient transport state by the time those tokens are cancelled.

Passing a caller token directly into an in-flight runtime mutation can strand the managed state in `Opening` or `Seeking`, or create ambiguity about whether the native side committed the operation.

## Decision

- The bounded command mailbox remains the single ordering authority for managed playback commands.
- A caller cancellation token is observed before the actor starts a command. If already cancelled, that command is skipped.
- Once a command starts, caller cancellation cancels only that caller's wait. It does not interrupt the runtime mutation.
- In-flight runtime mutations use a session-owned shutdown token.
- Session disposal atomically transitions the actor out of the active state, closes the mailbox, cancels the shutdown token, rejects pending commands, waits for the actor loop, then disposes the runtime exactly once.
- New commands racing with shutdown fail as `ObjectDisposedException`.
- A disposed session publishes a final `Closed` snapshot while preserving its latest epoch.

## Consequences

This creates a clear commit boundary for commands and prevents transient playback state from depending on UI token lifetime. Future native interrupt support can be wired to the session shutdown token, while command supersession (for example rapid seek coalescing) must be modeled explicitly rather than abusing caller cancellation.
