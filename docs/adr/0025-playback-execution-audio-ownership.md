# ADR 0025: PlaybackExecution owns optional audio generations

- Status: Accepted
- Date: 2026-10-18

## Context

The audio branch now has independent Decode -> Process -> Render workers, explicit output-target negotiation, render-thread Pause/Resume control, a portable hardware-clock snapshot, and a clock-discipline policy. The product execution owner still starts only video, however, so those pieces are not yet governed by the same playback epoch, seek boundary, or lifecycle as the video branch.

Simply changing the demux selection from `{video=true, audio=false}` to `{true, true}` is not sufficient. Audio output preparation chooses an endpoint-specific target format before conversion starts, a seek may need to renegotiate that endpoint, Pause must synchronously freeze an already-open device, and a paused seek must not let a newly-created output begin presenting samples while the authoritative `PlaybackClock` remains paused.

`PlaybackExecution` also must remain independent of FFmpeg and WASAPI concrete types. Those adapters belong at composition edges, not in execution ownership.

## Decision

1. `PlaybackExecution` becomes track-aware. `PlaybackMediaSession` exposes read-only `has_video()` and `has_audio()` capability queries backed by the decoders selected during open.
2. Audio execution is optional dependency injection. The existing video-only constructor remains valid. Enabling audio requires both an `IAudioOutputProvider` and an `AudioFrameProcessorFactory`; the pair is rejected if only one side is supplied.
3. The output provider prepares a fresh `AudioOutputPlan` for every playback generation. This binds endpoint selection and target format to the same generation and naturally renegotiates after seek/discontinuity.
4. The processor factory receives only the selected portable `AudioFormat`. `PlaybackExecution` therefore does not depend on `FfmpegAudioFrameConverter`, libswresample, WASAPI, or platform types.
5. Route selection contains exactly the branches that have consumers. Video is activated when the session has video. Audio is activated only when the session has audio and audio execution dependencies are configured. Audio-only generations are valid.
6. Both decoder branches are started before downstream processing so the single demux owner can feed bounded per-stream packet queues without one selected stream lacking a consumer.
7. Audio Decode and Process may start while logical playback is stopped/paused, but Audio Render does not. The processed stream is retained as a pending bounded stream until `PlaybackClock` enters Running. Starting/resuming playback then starts or resumes the render worker.
8. This delayed render start is the paused-seek invariant: a seek performed while paused may decode/process into bounded queues, but the replacement endpoint is not even opened until Resume.
9. Pause freezes an active audio renderer first and only then pauses `PlaybackClock`. Resume makes `PlaybackClock` Running first and only then starts/resumes audio output. Failure in either control path faults the execution and tears down the generation.
10. Natural end is reached only when every active branch is drained. Inactive branches do not participate in the end condition.
11. Generation teardown is downstream-to-upstream across both branches: audio/video sinks, schedulers/processors, decoders, then the shared packet-routing generation. Audio output and processor objects are generation-scoped and destroyed after shutdown.
12. `PlaybackExecutionSnapshot` reports whether video/audio are active plus audio decode/process/render state, render statistics, and the portable audio output position. This gives the next synchronization layer what it needs without exposing the device adapter.

## Consequences

- Video-only callers remain source-compatible.
- Audio-only media can be exercised through the same execution owner, which gives deterministic integration coverage without requiring a combined A/V test asset or physical audio endpoint.
- Paused seek cannot accidentally start a replacement audio device.
- Endpoint changes that occur across a discontinuity can be picked up by the next provider preparation.
- Bounded backpressure remains the mechanism that limits pre-render audio while paused; no unbounded staging is introduced.
- Future audio-clock discipline can consume `PlaybackExecutionSnapshot::audio_position` without reaching into `AudioRenderWorker` or a platform adapter.

## Deferred

- Applying `AudioClockDiscipline` recommendations to `PlaybackClock` and defining the final audio-master A/V synchronization policy.
- Long-term drift/rate correction beyond thresholded clock rebases.
- Runtime/public-C-ABI construction of concrete Windows audio dependencies.
- Explicit endpoint switching/change notifications and recovery after device loss.
- `IAudioClient3` low-latency period policy, exclusive mode, and bitstream output.
