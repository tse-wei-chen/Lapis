# ADR-0010 — Output experiment stays outside MediaRuntime

Status: accepted for M1d, 2026-10-20.

## Context

M1c had portable decoded RGBA/PCM buffers but no actual presentation handoff.
SDL2 provides a small video/audio smoke path without locking the eventual product to SDL.
A headless sandbox has SDL2 runtime but no display/audio device or development headers.

## Decision

Introduce opt-in `bdmv_sdl_preview`, a standalone consumer of the **existing C ABI**.
The core must not import SDL symbols/headers or depend on SDL availability. The preview
uses runtime dynamic loading solely to test this integration without extra SDK install.
Regular builds do not include the preview by default. SDL dummy integration is opt-in.

Use deterministic `VideoTiming` in portable Core as presentation *policy*, driven by
caller-supplied media clock values. Presenter chooses and supplies the actual audio
or monotonic clock; Core does not fetch OS time or own presentation devices.

## Consequences

- No MediaRuntime ABI version change for this experiment.
- Preview uses CPU RGBA textures and queued PCM, not HDR, zero-copy, hardware clock or
  Blu-ray/BD-J overlay integration.
- Diagnostic audio playhead calculated from SDL queued bytes excludes backend/device
  latency; it must never be presented as proof of precise A/V synchronization.
- SDL2 runtime shim/manual public ABI usage is for the prototype **only**. A production
  adapter needs vendor-supported headers, lifecycle/error handling, and OS/device tests.
- Optional executable can be replaced without touching decode Source/Session or C ABI.
