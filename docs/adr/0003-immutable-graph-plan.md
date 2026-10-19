# ADR 0003: Immutable GraphPlan

- Status: Accepted
- Date: 2026-10-17

## Context

Allowing arbitrary node insertion/removal/reconnection while media threads are running creates difficult lifetime, synchronization, and rollback problems. Keeping a second executable graph model in the managed control plane would also create two sources of truth for node kinds, adapter choices, and runtime lifetime rules.

## Decision

A media pipeline configuration is represented by an immutable native `GraphPlan` owned by `MediaRuntime/Core`.

Managed Application code owns product intent and playback policy. It does not model concrete demux, decoder, processor, renderer, or sink nodes. Native `GraphBuilder` resolves those requirements against source, device, and adapter capabilities and produces the single executable plan.

When playback requirements change (for example track selection, output mode, HDR policy, or a future processor):

1. build a new native plan from the requested policy and available capabilities;
2. validate and prepare resources;
3. warm the new graph if needed;
4. atomically switch the active plan;
5. retire the previous graph after in-flight work is safe.

Source discovery first attaches Source -> Demuxer -> selected Decoder topology. ADR 0035 adds a pure native execution-graph compilation step that validates reachable decoders, adds the selected processing/scheduling/output nodes, and makes the resulting immutable plan the playback generation's authoritative branch-activation record. Full graph-node factory execution and atomic warm-and-swap remain deferred rather than being claimed by the current implementation.

## Consequences

There is one source of truth for executable graph topology. Application code remains stable when FFmpeg, D3D, WASAPI, hardware decoder, or future plugin adapter implementations change. Graph transitions remain explicit state changes rather than uncontrolled live mutation.
