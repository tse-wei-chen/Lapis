# ADR 0001: Managed control plane and native media data plane

- Status: Accepted
- Date: 2026-10-17

## Context

The playback hot path deals with packets, frames, audio buffers, GPU resources, callback timing, and renderer synchronization. Repeated managed/native crossings at frame or packet frequency would couple the product to native implementation details and complicate lifetime management.

## Decision

Lapis separates responsibilities:

- managed code owns product behavior, commands, settings, library, capabilities, and orchestration;
- native C++ owns packet/frame/audio/GPU hot paths;
- the boundary is a versioned stable C ABI carrying low-frequency commands, snapshots, handles, and plain value types;
- every session has a stable 128-bit session id and every command that operates on an existing timeline carries an epoch so stale work can be rejected at the native boundary;
- media locator kind is explicit (`File`, `Directory`, `Uri`, or `OpticalDisc`) rather than inferred from a string;
- FFmpeg, libbluray, GPU SDK, and codec types are forbidden in managed contracts.

## Consequences

The native runtime can change adapters without forcing UI/domain rewrites. Interop is explicit and testable, high-frequency media objects remain native-to-native, and managed/native binary mismatches can be detected through the ABI version before creating playback state.
