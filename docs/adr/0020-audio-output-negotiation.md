# ADR 0020: Audio output negotiation precedes stream rendering

- Status: Accepted
- Date: 2026-10-17

## Context

The audio branch already separates decode, sample conversion, and device rendering. `AudioRenderWorker` consumes an `IAudioOutput`, while `FfmpegAudioFrameConverter` needs its output `AudioFormat` at construction time.

A concrete device stream therefore cannot be the component that first decides the target format after processing has started. On Windows shared mode, `IAudioClient::GetMixFormat` exposes the audio-engine mix format before stream initialization, and `IsFormatSupported` can verify an exact reconstructed stream format.

There is also a device-identity race if negotiation queries the current default endpoint but the later `IAudioOutput::open` independently resolves the default endpoint again. A default-device change between those operations could make processing target one endpoint while rendering opens another.

## Decision

1. Introduce `IAudioOutputProvider` as a pre-render negotiation boundary. It returns an `AudioOutputPlan` containing an exact Lapis-owned `target_format` plus an unopened `IAudioOutput` instance.
2. `AudioOutputPlan` is portable. No `IMMDevice`, `IAudioClient`, `WAVEFORMATEX`, endpoint handle, or other WASAPI type crosses the provider boundary.
3. `WasapiSharedAudioOutputProvider` resolves the default `eRender` / `eMultimedia` endpoint, captures its stable endpoint ID, activates a temporary `IAudioClient`, and calls `GetMixFormat` before any render worker starts.
4. The returned WASAPI mix descriptor is parsed into `AudioFormat`. Only representations and channel layouts that Lapis can model exactly are accepted. Reduced-valid-bit integer formats such as 24 valid bits in a 32-bit container are rejected in this slice.
5. The parsed `AudioFormat` is converted back to `WAVEFORMATEXTENSIBLE` and checked with shared-mode `IsFormatSupported`. `S_FALSE` / closest-match results are rejected; negotiation never silently accepts a different format.
6. The prepared `WasapiSharedAudioOutput` is bound to the endpoint ID captured during negotiation. Its later `open(target_format)` reacquires that specific endpoint rather than re-resolving the current default device.
7. COM apartment ownership is explicit. `RPC_E_CHANGED_MODE` means the caller thread is already initialized in another apartment and is usable; Lapis calls `CoUninitialize` only when its own `CoInitializeEx` call succeeded.
8. `IAudioOutput` remains a stream-transport boundary. It does not gain device discovery, mix-format queries, resampling policy, or endpoint-selection policy.

## Consequences

- Upstream orchestration can construct `FfmpegAudioFrameConverter` from `AudioOutputPlan::target_format` before starting Audio Decode -> Process -> Render.
- Device discovery and stream submission stay independently replaceable.
- A default-device switch cannot silently split negotiation and rendering across two endpoints.
- Format conversion policy remains visible and testable instead of being hidden in WASAPI.
- The provider can later grow endpoint enumeration/selection without changing `AudioRenderWorker` or `IAudioOutput` submission semantics.

## Deferred

- Explicit endpoint enumeration, persisted user selection, and default-device change notifications.
- Fallback target selection when the engine mix representation is not directly representable by `AudioFormat`.
- `IAudioClient3` shared-engine period selection and low-latency policy.
- Output pause/resume control and render-thread control mailbox.
- `IAudioClock` hardware-clock discipline and A/V synchronization.
- Exclusive PCM and encoded bitstream/passthrough negotiation.
