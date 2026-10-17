# Architecture overview

Lapis is designed as a media platform rather than a UI wrapped around one decoder stack.

```text
UI / future frontends
        |
        v
Application control plane
  - command mailbox / playback session / policy
        |
        | stable C ABI / adapter boundary
        v
Native MediaRuntime
  - immutable GraphPlan / GraphBuilder
  - source / demux adapters
      active DecodeGeneration
        DemuxPump -> bounded selected-stream packet queues -> decoders
  - decoded media contracts
      VideoFrame / AudioFrame (third-party-independent lifetime + metadata)
  - PlaybackExecution video generation owner
      VideoDecode -> VideoProcess -> VideoScheduler -> VideoRender
           |              |                |               |
      bounded frame  bounded frame    bounded present  IVideoPresenter
          stream          stream           stream          adapter
  - prepared audio execution branch
      IAudioOutputProvider -> target AudioFormat + bound IAudioOutput
                                  |
      AudioDecode -> AudioProcess -> AudioRender -> IAudioOutput
           |              |              |              |
      bounded frame  libswresample  partial submit   platform adapter
          stream         adapter       + drain       (WASAPI shared)
                                      + pause/resume
                                      + clock snapshot
  - authoritative PlaybackClock
        |
        +---- media taps ----> AIHost (future, non-blocking)
```

## Invariants

1. UI does not decode or render media; managed code does not manipulate packet/frame/GPU hot-path objects.
2. Third-party libraries remain adapters at the edge; their types do not enter core/managed contracts.
3. `MediaTime` is canonical and every discontinuity advances `PlaybackEpoch`.
4. Playback commands have one logical owner; executable graph ownership stays native.
5. Every cross-thread media queue is bounded and stop-aware; no stage may create an unbounded backlog.
6. Every timed sample carries `PlaybackEpoch` + canonical `MediaTime`, and stale epochs are revalidated at asynchronous boundaries.
7. `PlaybackClock` is the single authoritative playback-time model and atomically binds state, epoch, time, and revision.
8. Decode, processing, scheduling, and render/presenter responsibilities remain separate stages.
9. Pixel/color conversion is a Video Processor responsibility, not a presenter responsibility.
10. Graphics APIs live behind presenter adapters; decode/process/scheduler core does not depend on D3D/DXGI types.
11. `PlaybackExecution` owns active product worker-generation lifecycle. Active packet routing exists only for streams with active consumers.
12. Video startup is Route -> Decode -> Process -> Scheduler -> Render; shutdown is Render -> Scheduler -> Process -> Decode -> Route.
13. Pause keeps the active generation alive and preserves in-flight media/device queues; seek/close tear the generation down before media mutation.
14. `av_read_frame` has one owner. Selected demux streams fan out through independent bounded packet queues; decoders never compete for the demuxer.
15. Packet-routing generations and decoder buffers are discarded across seek; no queue or buffered frame crosses a discontinuity.
16. Decoded video/audio storage is hidden behind Lapis-owned frame/buffer contracts; `AVFrame` never crosses the adapter boundary.
17. Audio decoding, device-format conversion, device negotiation, and device output are separate responsibilities; WASAPI consumes processed Lapis-owned audio, not FFmpeg-native frames.
18. AI observes playback and never sits in the blocking playback path.
19. Audio output adapters do not silently resample, repack, reorder channels, or hide unbounded staging. Device-format policy stays in the processing/orchestration boundary.
20. Audio target-format negotiation happens before worker startup. A prepared output plan binds the chosen `AudioFormat` and the eventual `IAudioOutput` to the same endpoint identity.
21. Audio device control executes on the render thread. Pause/Resume only interrupt waits and preserve the current sample cursor/device queue; destructive reset remains reserved for discontinuities.
22. Audio hardware timing crosses the platform boundary only as a portable media-position + monotonic-correlation snapshot. QPC, COM, and `IAudioClock` never enter execution/core contracts.

## Current implementation slice

The current slice opens local files through `IMediaByteStream`, feeds FFmpeg via custom AVIO, and keeps FFmpeg types inside adapters. Media open discovers/prepares video and audio decoders but does not start the demux pump. Execution explicitly creates an immutable routing generation containing only streams with active consumers.

The active product execution path is still video-only and flows Video Decode -> Process -> Scheduler -> Render under the authoritative playback clock. In parallel, the audio branch now has independently tested Decode -> Process -> Render stages: `AudioDecodeWorker` produces Lapis-owned `AudioFrame` values, `FfmpegAudioFrameConverter` performs drainable libswresample conversion behind `IAudioFrameProcessor`, and `AudioRenderWorker` drives an `IAudioOutput` through bounded partial submission, stop-aware readiness waits, drain, reset, and synchronous render-thread-owned Pause/Resume control.

Audio output target selection has its own portable `IAudioOutputProvider` boundary. A provider prepares an `AudioOutputPlan` before processing starts, containing the exact `AudioFormat` that the converter should produce plus an unopened output already bound to the same physical/logical endpoint used for negotiation. This prevents default-device changes from splitting format discovery and rendering across different endpoints.

`AudioRenderWorker::pause()` and `resume()` act as synchronous control-plane acknowledgements rather than direct device calls. A dedicated wait-interrupt source wakes bounded input waits, device backpressure waits, or EOF drain; the render thread then applies `IAudioOutput::pause/resume` and acknowledges the control revision. Concurrent public Pause/Resume calls are serialized until the current acknowledgement completes, preventing a later command from overwriting an earlier revision. A partial frame retains its sample cursor and EOF retains its drain phase across Pause.

On Windows, `WasapiSharedAudioOutputProvider` resolves the default multimedia render endpoint, reads its shared engine mix format with `GetMixFormat`, converts that descriptor into the Lapis audio model, verifies the reconstructed format with exact shared-mode `IsFormatSupported`, and returns a `WasapiSharedAudioOutput` bound to the captured endpoint ID. The output maps Pause to `IAudioClient::Stop` and Resume to `Start`, preserving queued endpoint samples and stream position; reset remains Stop + Reset for discontinuities.

`WasapiSharedAudioOutput` now also acquires `IAudioClock`. The first successfully released sample prefix maps stream position zero to its canonical media timestamp. Render-thread refreshes combine `IAudioClock::GetPosition` with `GetFrequency`, bridge the returned QPC correlation into the same monotonic domain used by `PlaybackClock`, and publish a coherent `AudioOutputPosition` snapshot through atomics. `position()` never calls COM and can be read from a future synchronization owner without crossing device-thread affinity.

This hardware snapshot is an input to future clock discipline, not yet a second authoritative playback clock. `PlaybackClock` remains the product time model until an explicit A/V master-clock policy defines how hardware samples rebase or discipline it.

Blu-ray navigation, explicit endpoint selection/change notifications, fallback target-format policy for non-representable engine mix formats, `PlaybackExecution` ownership of the audio branch, audio-master clock discipline and A/V synchronization, `IAudioClient3` low-latency period policy, exclusive/bitstream output, concrete D3D11/DXGI presentation, GPU decode/processing, full color/HDR metadata, subtitles, AI taps, FFmpeg interrupt callbacks for blocking sources, and public-C-ABI delegation to `PlaybackExecution` remain deliberately outside this slice.
