# ADR 0014: Packet routing generations follow active decode consumers

- Status: Accepted
- Date: 2026-10-17

## Context

The first packet router started during `PlaybackMediaSession::open`, before any execution worker existed. That was tolerable for one video stream because the bounded video queue merely prefetched and then backpressured. It does not scale to audio: selecting an audio stream before an Audio Worker/Sink chain is active can fill the audio queue and stop the single demux pump, indirectly stalling video.

A seek also needs one unambiguous lifecycle boundary so packet queues, decoder buffers, and `PlaybackEpoch` never span a discontinuity.

## Decision

1. Opening media prepares demuxers and available video/audio decoder adapters but does not start packet routing.
2. `PlaybackMediaSession::start_decode_generation` explicitly selects only streams that have active consumers.
3. A routing generation is stopped before seek, close, or worker-generation replacement. Decoder buffers are flushed when the routing generation ends.
4. `PlaybackExecution` starts the current video-only routing generation before Video Decode and stops it after all video workers have joined.
5. `AudioDecodeWorker` mirrors the bounded, stop-aware video decode worker contract and can participate in a future combined audio+video generation.
6. `PlaybackMediaSession` can now decode video and audio concurrently from one router when both are explicitly selected.
7. Media discovery records both available video and audio decoder nodes in the opened native graph; availability does not imply execution activation.
8. Direct low-level worker tests own routing-generation start/stop explicitly rather than relying on hidden prefetch from media open.

## Consequences

- No selected packet queue exists without a corresponding decode consumer by construction at the execution layer.
- Audio can be activated later without changing demux ownership or seek semantics.
- Future audio/video execution startup must select both streams as one generation and start both decode consumers promptly.
- Device-format processing, audio sink consumption, and audio-master clock discipline remain separate milestones.
