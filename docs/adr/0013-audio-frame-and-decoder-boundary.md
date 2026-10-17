# ADR 0013: Decoded audio uses a Lapis-owned frame contract before output integration

- Status: Accepted
- Date: 2026-10-17

## Context

After packet ownership moved to `FfmpegPacketRouter`, an audio decoder can be introduced without competing with video for demux packets. The next risk is allowing FFmpeg sample formats, channel-layout types, or `AVFrame` lifetime to leak into later WASAPI and playback-clock code.

Audio output is deliberately not connected yet. Registering an audio stream in the active playback packet router before an audio consumer/sink exists would eventually fill its bounded queue and backpressure video.

## Decision

1. `AudioFrame`, `IAudioBuffer`, `AudioFormat`, sample-format, packing, and channel-position types are Lapis-owned native core contracts.
2. `FfmpegAudioDecoder` remains inside the FFmpeg adapter boundary and converts decoded `AVFrame` metadata into the Lapis contract.
3. Decoded storage keeps FFmpeg buffer references alive behind `IAudioBuffer`; consumers receive read-only plane views and never own an `AVFrame*`.
4. Audio frames carry canonical `MediaTime`, duration, and optional source timestamp/time-base metadata.
5. Planar and interleaved decoder output are both representable. Normalization to a device format is the responsibility of a future Audio Processor, not the decoder or WASAPI sink.
6. Channel order is preserved as an ordered list of Lapis channel positions. If the source/decoder exposes only a channel count with `AV_CHANNEL_ORDER_UNSPEC`, the FFmpeg adapter resolves the standard default layout for that count and marks `AudioFormat.channel_layout_inferred = true`; explicit layouts keep that flag false. Unsupported channel identifiers remain `Unknown` rather than leaking FFmpeg enums.
7. This milestone adds audio stream discovery and decoder tests only. `PlaybackMediaSession` continues activating only the video stream, so existing video playback cannot be stalled by an unconsumed audio queue.
8. Audio becomes part of the active execution generation only when an Audio Worker/Processor/Sink chain exists to consume it.

## Consequences

- WASAPI can distinguish source-declared channel positions from an adapter-inferred conventional layout.
- WASAPI will receive a stable, third-party-independent audio model.
- A later libswresample stage can normalize rate/sample format/channel layout without changing decoder ownership.
- Existing video playback behavior is unchanged for files that also contain audio.
