# ADR 0017: Audio processing is a drainable stage before device output

- Status: Accepted
- Date: 2026-10-17

## Context

Decoded audio can use arbitrary sample formats, packing, sample rates, and channel layouts, while a concrete output device will accept a narrower negotiated format. Putting conversion inside WASAPI would make conversion quality, buffering, timestamps, and channel policy backend-specific and would force every future audio backend to solve the same problem independently.

Unlike the current video processor, sample-rate conversion can buffer input samples that require future input. End-of-input therefore cannot be represented by a single stateless `process(frame)` call.

## Decision

1. `IAudioFrameProcessor` is the third-party-independent conversion contract between Audio Decode and Audio Render.
2. The contract exposes a fixed `output_format()`, `process()`, repeated `drain()` after input EOF, and `reset()` for epoch discontinuities.
3. `AudioProcessWorker` owns one processor for an epoch-scoped generation and connects input/output with another bounded `AudioFrameStream`.
4. Startup resets processor state before accepting the new epoch. Interrupted or failed generations reset again so buffered samples cannot cross seek boundaries.
5. Processor EOF is reached only after `drain()` returns `Drained`; any frames produced while draining are published before the output stream closes.
6. The first adapter, `FfmpegAudioFrameConverter`, is isolated under `/process/ffmpeg/` and uses libswresample for sample-format conversion, interleaved/planar conversion, sample-rate conversion, and channel-layout conversion.
7. Core `AudioChannel` ordering is translated to an explicit FFmpeg custom `AVChannelLayout`; FFmpeg layout objects do not escape the adapter boundary.
8. The converter rejects decoded-format changes while buffered state exists rather than silently rebuilding the resampler mid-generation.
9. Processed frame duration is derived from produced samples at the output sample rate. Canonical output timestamps compensate for the resampler delay; drain frames continue from the last processed output timestamp.
10. `AudioFrameStream` remains the shared bounded/epoch-tagged transport type for Decode -> Process -> Render; downstream output code does not know about libswresample.

## Consequences

- WASAPI can remain a narrow device adapter that consumes an already-negotiated format instead of becoming a hidden resampler.
- Other future audio backends can reuse the same processing stage and conversion policy.
- Resampler latency and flush behavior are explicit and testable.
- A 48 kHz S16 decoded fixture is integration-tested through Decode -> Process into 44.1 kHz F32, including resampler drain and epoch reset.
- `swresample` becomes an explicit pinned FFmpeg/vcpkg feature.

## Deferred

- Output-device capability discovery and selection of the processor target format.
- Concrete WASAPI shared/exclusive output.
- `PlaybackExecution` ownership of Audio Decode -> Process -> Render.
- Volume/mute processing, user channel-remap policy, passthrough/bitstream audio, and drift compensation.
