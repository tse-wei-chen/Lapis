# ADR 0003: Immutable GraphPlan

- Status: Accepted
- Date: 2026-10-17

## Context

Allowing arbitrary node insertion/removal/reconnection while media threads are running creates difficult lifetime, synchronization, and rollback problems.

## Decision

A media pipeline configuration is represented by an immutable `GraphPlan`.

When playback requirements change (for example track selection, output mode, HDR policy, or a future processor):

1. build a new plan from available capabilities;
2. validate and prepare resources;
3. warm the new graph if needed;
4. atomically switch the active plan;
5. retire the previous graph after in-flight work is safe.

The current foundation implements the immutable plan description and capability validation. Native graph execution and atomic switching come later.

## Consequences

Graph transitions become explicit state changes rather than uncontrolled mutation. This costs some preparation work but greatly reduces runtime race conditions and makes fallback plans possible.
