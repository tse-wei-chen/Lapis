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
      DemuxPump -> bounded selected-stream packet queues -> decoders
  - PlaybackExecution generation owner
      DecodeWorker -> ProcessWorker -> SchedulerWorker -> RenderWorker
           |              |                |               |
      bounded frame  bounded frame    bounded present  IVideoPresenter
          stream          stream           stream          adapter
  - authoritative PlaybackClock
  - future audio / subtitle hot paths
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
11. `PlaybackExecution` is the sole owner of video worker-generation lifecycle.
12. Generation startup is Decode -> Process -> Scheduler -> Render; shutdown is Render -> Scheduler -> Process -> Decode.
13. Pause keeps the generation alive and relies on bounded backpressure; seek/close tear it down before media mutation.
14. `av_read_frame` has one owner. Selected demux streams fan out through independent bounded packet queues; decoders never compete for the demuxer.
15. Packet-routing generations are stopped and replaced across seek; packet queues are never reopened across a discontinuity.
16. AI observes playback and never sits in the blocking playback path.

## Current implementation slice

The current slice opens local files through `IMediaByteStream`, feeds FFmpeg via custom AVIO, and keeps FFmpeg types inside adapters. `FfmpegPacketRouter` now owns sequential demux reads and fans selected streams into bounded queues, removing decoder ownership of `av_read_frame` before audio is introduced. Video decode consumes its routed stream, then `VideoProcessWorker` normalizes software-decoded system-memory frames through the isolated libswscale adapter into BGRA8 before scheduling. `PlaybackExecution` owns Decode -> Process -> Scheduler -> Render, the authoritative clock, bounded stream generations, epoch validation, and discontinuity stop ordering.

Blu-ray navigation, audio/WASAPI and audio-clock discipline, concrete D3D11/DXGI presentation, GPU decode/processing, full color/HDR metadata, subtitles, AI taps, FFmpeg interrupt callbacks for blocking sources, and public-C-ABI delegation to `PlaybackExecution` remain deliberately outside this slice.
