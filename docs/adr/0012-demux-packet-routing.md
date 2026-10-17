# ADR 0012: Demux packet reads have one owner and fan out through bounded stream queues

- Status: Accepted
- Date: 2026-10-17

## Context

The first video-only slice let `FfmpegVideoDecoder` call `av_read_frame` indirectly through `FfmpegDemuxer`. That is safe with one selected stream, but it cannot scale to audio or subtitles: independent decoders reading the same demuxer would race, consume packets intended for one another, and silently discard non-target streams.

Audio support therefore requires a packet-routing boundary before a second decoder is introduced.

## Decision

1. `FfmpegPacketRouter` is the sole runtime owner of sequential packet reads from an opened FFmpeg demuxer.
2. One demux pump routes packets for selected streams into independent bounded, stop-aware queues.
3. Decoders consume only their own stream queue and never call `av_read_frame` themselves.
4. Unselected streams are discarded by the router rather than by a decoder.
5. A routing generation is immutable after start: selected stream indexes and queue capacity do not change while it is running.
6. Stop requests interrupt bounded queue waits. The pump is joined before seek or demux destruction.
7. Seek stops the current routing generation, seeks/flushes decoder state, then starts fresh queues. Queues are never reopened across a discontinuity.
8. Packet ownership remains entirely inside the native FFmpeg adapter boundary. `AVPacket` never enters core, managed contracts, or UI code.
9. The current local-file source does not yet install an FFmpeg interrupt callback for a blocking `av_read_frame`; that is required before network/blocking sources are enabled.

## Consequences

- Video, audio, and subtitle decoders can coexist without stealing one another's packets.
- Backpressure is explicit per selected stream and memory use remains bounded.
- A selected stream must have an active consumer or the demux pump may intentionally backpressure; stream selection and consumer activation must therefore be coordinated by the execution generation.
- Audio decode can now be added without changing packet ownership again.
