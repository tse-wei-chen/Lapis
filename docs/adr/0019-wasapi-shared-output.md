# ADR 0019: WASAPI shared output is an event-driven platform adapter

- Status: Accepted
- Date: 2026-10-17

## Context

`IAudioOutput` now supports exact format negotiation, non-blocking partial sample-frame submission, stop-aware readiness waits, drain, reset, and thread-safe position snapshots. The next audio slice needs a real Windows sink without moving sample conversion or device policy into the playback core.

WASAPI shared mode exposes a finite endpoint buffer whose writable capacity changes independently of decoded frame boundaries. Event-driven rendering is therefore a natural fit for the existing partial-submit contract, but only if the adapter remains a narrow device boundary.

## Decision

1. `WasapiSharedAudioOutput` is a Windows-only `IAudioOutput` adapter under `/audio/wasapi/`. No COM, `WAVEFORMATEX`, `IMMDevice`, `IAudioClient`, or `IAudioRenderClient` type may cross that adapter boundary.
2. The first slice opens the default `eRender` / `eMultimedia` endpoint. Explicit endpoint enumeration and selection remain application/device-policy work for a later slice.
3. The adapter consumes an already-processed exact interleaved PCM format. It supports `U8`, `S16`, `S32`, and `F32` representations that can be represented by `WAVEFORMATEXTENSIBLE`. Planar input, unknown channels, duplicate/non-canonical channel order, and unsupported representations are rejected.
4. Shared-mode `IsFormatSupported` must return exact support. A closest-match response is rejected; the adapter does not enable hidden Windows sample-rate or channel conversion.
5. Shared event-driven initialization uses `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` with zero buffer duration and periodicity. The event handle is installed before playback starts.
6. `try_submit` never blocks. It reads current padding, obtains only the currently writable prefix from `IAudioRenderClient`, copies that prefix, releases it, and reports the exact accepted sample-frame count.
7. The stream starts only after the first audio prefix has been queued, avoiding an intentional silent lead-in. Further pacing waits on the WASAPI event, with a separate stop event bridged from `std::stop_token`; no polling loop is introduced.
8. EOF drain waits until endpoint padding reaches zero. Discontinuity reset performs `Stop` + `Reset`, invalidates the position revision, and discards queued endpoint samples before reuse.
9. `position()` remains a thread-safe queue-based presentation estimate derived from submitted media time minus current endpoint padding. It is not yet the authoritative audio hardware clock.
10. Device-independent unit coverage validates the Lapis `AudioFormat` -> `WAVEFORMATEXTENSIBLE` boundary without requiring an audio endpoint on CI runners.

## Consequences

- WASAPI stays a replaceable output adapter instead of becoming a second audio-processing pipeline.
- Partial submission maps directly to endpoint capacity and does not require an unbounded staging buffer.
- Seek/close can interrupt device waits through the existing render-worker stop token.
- Unsupported layouts fail explicitly instead of being silently reordered or converted.
- The adapter can be introduced before `PlaybackExecution` owns audio, keeping pause semantics and audio-master-clock policy out of this change.

## Deferred

- Endpoint enumeration, persisted device selection, and hot-plug/default-device change handling.
- Device mix-format discovery and target-format policy for `FfmpegAudioFrameConverter`.
- `IAudioClient3` low-latency shared-mode period selection.
- Output pause/resume lifecycle and audio hardware-clock discipline.
- `PlaybackExecution` ownership of Audio Decode -> Process -> Render and A/V synchronization.
- Exclusive PCM, IEC 61937 / bitstream passthrough, and protected-content/licensed codec paths.
