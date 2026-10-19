# Architecture overview

Lapis is designed as a media platform rather than a UI wrapped around one decoder stack.

```text
Lapis.Desktop.Avalonia (MVVM / NativeControlHost)
        |
        v
Lapis.Desktop.Hosting
  - host command serialization + HWND lifetime / presentation port
        |
        v
Application control plane
  - command mailbox / playback session / policy
        |
        | stable C ABI / adapter boundary
        v
Native MediaRuntime composition root
  - C ABI session -> PlaybackExecution
  - Windows: D3D11/DXGI presenter (headless when no HWND is bound)
  - Other platforms: headless presenter until a local output adapter exists
  - Windows default audio -> WASAPI shared provider
        |
        v
Native MediaRuntime core
  - immutable GraphPlan / GraphBuilder
  - source / demux adapters
      MediaSourceSession: byte stream + optional INavigationController
        title / playlist / chapter / angle / menus / overlays (contract only)
      active DecodeGeneration
        DemuxPump -> bounded selected-stream packet queues -> decoders
  - decoded media contracts
      VideoFrame / AudioFrame (third-party-independent lifetime + metadata)
  - PlaybackExecution generation owner
      VideoDecode -> VideoProcess -> VideoScheduler -> VideoRender
           |              |                |               |
      bounded frame  bounded frame    bounded present  IVideoPresenter
          stream          stream           stream          adapter

      IAudioOutputProvider -> target AudioFormat + bound IAudioOutput
                                  |
      AudioDecode -> AudioProcess -> AudioRender -> IAudioOutput
           |              |              |              |
      bounded frame  libswresample  partial submit   platform adapter
          stream         adapter       + drain       (WASAPI shared)
                                      + pause/resume
                                      + clock snapshot
                                             |
                                             v
                                  AudioClockSyncWorker
                                  (sample only while running)
                                             |
                                             v
                                  AudioClockDiscipline
                                  (ignore/maintain/rebase)
                                             |
                                             v
                         PlaybackClock::try_rebase(expected snapshot)
                                             |
  - authoritative PlaybackClock <-------------+
        |
        +---- video scheduler timing
        |
        +---- media taps ----> AIHost (future, non-blocking)
```

## Desktop frontend: Avalonia

The first product page is a **library**: `Lapis.Library` indexes each local
file or complete BDMV root behind `IMediaLibrary`. Its optional
`DiscArtworkLocator` searches only BDMV/META/DL; Avalonia owns the
background decoded thumbnails and their lifetimes, keeping the catalog
format and playback pipeline independent of image decoding. Its first JSON persistence
adapter does not manipulate media, and `LibraryViewModel` is not a playback
session. Selecting one card opens the distinct Watch page with the existing
serialized playback actor (ADR 0041). MPLS filenames shown in the disc sidebar
are read-only previews, not playable playlist navigation.

`Lapis.Desktop.Avalonia` is the only desktop presentation adapter. Its
`NativeControlHost` owns an Avalonia proxy HWND and a persistent GPU child
HWND. The UI-neutral `Lapis.Desktop.Hosting` serializes playback and surface
binding commands; native detach/render-thread shutdown must finish before the
GPU child HWND is destroyed. The ViewModel uses `IPlayerPlaybackService`,
not FFmpeg or D3D11 types. Native Win32 soak and real Avalonia integration
tests now replace the retired WinForms test shell.

The actor now also consumes native execution snapshots via the optional
`IPlaybackRuntimeObserver` port. It polls only while Playing/Paused, off the UI
thread, and posts observations into its existing bounded command mailbox.
The actor rejects mismatched session IDs or playback epochs, negative positions,
transitional states, and same-epoch Play/Pause regressions; terminal Ended or
Faulted is published with the native position. Open-result duration is carried
through the same `PlaybackSnapshot` consumed by UI. Poll cancellation and task
join finish before native runtime destruction; no timer or IO lifetime crosses
session disposal. The first desktop UI uses this to display position/duration
and to seek by dragging the progress slider.

## Invariants

1. UI does not decode or render media; managed code does not manipulate packet/frame/GPU hot-path objects.
2. Third-party libraries remain adapters at the edge; their types do not enter core/managed contracts.
3. `MediaTime` is canonical and every media discontinuity advances `PlaybackEpoch`.
4. Playback commands have one logical owner; executable graph ownership stays native.
5. Every cross-thread media queue is bounded and stop-aware; no stage may create an unbounded backlog.
6. Every timed sample carries `PlaybackEpoch` + canonical `MediaTime`, and stale epochs are revalidated at asynchronous boundaries.
7. `PlaybackClock` is the single authoritative playback-time model and atomically binds state, epoch, time, and revision.
8. Decode, processing, scheduling, render/presenter, and synchronization responsibilities remain separate stages.
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
23. Hardware-clock observations are filtered by a platform-independent discipline policy before they may change `PlaybackClock`; ordinary device sampling must not itself create playback-clock revisions or scheduler wakeups.
24. `PlaybackExecution` activates only media branches that have both a selected track and a configured consumer. Audio-only playback is valid, and a paused seek may fill bounded Audio Decode/Process queues but may not open the replacement endpoint until Resume.
25. Asynchronous audio-clock corrections are compare-applied against the exact `PlaybackClock` state + epoch + revision that produced them. A concurrent Pause/Resume/Seek invalidates the correction instead of being overwritten.
26. Clock synchronization reads hardware timing only while the audio render worker is actively Running. Paused, drained, unavailable, or faulted render state never becomes a master-clock source.
27. Public C ABI transport commands are serialized per session and delegate to one `PlaybackExecution`; the ABI does not directly seek demux state or synthesize Play/Pause by mutating transport enums.
28. A seek while execution is Stopped may prepare a new epoch without starting workers. Starting that exact prepared epoch + position is allowed because starting presentation is not a second media discontinuity.

## Current implementation slice

The product desktop UI is now **Avalonia 11/MVVM** (`Lapis.Desktop.Avalonia`),
with a replaceable `IPlayerPlaybackService` and Windows-specific
`NativeControlHost` HWND adapter. The former WinForms shell is removed. Avalonia NativeControlHost has an airspace
boundary: transparent subtitles/menus must be composed into a proper GPU
surface rather than assumed to overlay an HWND (ADR 0040).

The source-selected Blu-ray chapter-seek control path resolves a chapter
ID to an immutable MediaTime through a separately versioned, read-only C ABI
extension. Only the serialized C# PlaybackSession actor may then invoke the
normal epoch-advancing Seek; the navigation lookup does not mutate libbluray
or launch/retire workers itself (ADR 0039). Full Title/Playlist replacement,
BD-J menu navigation and a chapter-picker UI are not implemented.

The current slice opens local files through `IMediaByteStream`, feeds FFmpeg via custom AVIO, and keeps FFmpeg types inside adapters. At each execution-generation boundary, a pure native compiler validates the selected decoders against the discovery graph, extends the reachable selected paths through processing/scheduling/sinks, and builds an immutable activation GraphPlan. PlaybackExecution derives active video/audio branches from that validated plan, publishes a locked value snapshot for diagnostics, and retires the plan only after worker shutdown. This is an intermediate invariant, not a general graph-node factory, plugin execution registry, or atomic warm-and-swap engine. Media open discovers/prepares video and audio decoders but does not start the demux pump. `PlaybackExecution` creates an immutable routing generation containing only streams with active consumers and owns both video and optional audio lifecycle under one `PlaybackEpoch`.

The public C ABI now owns a composition-level pipeline containing `PlaybackMediaSession` plus `PlaybackExecution`. `lapis_session_play`, `pause`, `seek`, and `close` call that execution owner rather than maintaining a parallel fake transport state machine. Per-session command serialization keeps concurrent result-returning calls ordered while the short-lived session mutex remains available to snapshots. Snapshot publication derives active time and natural Ended/Faulted state from `PlaybackExecutionSnapshot`.

The Windows C ABI composition root supplies a Win32 `D3D11VideoPresenter` behind the portable `IVideoPresenter` boundary. When a host binds an HWND through ABI v2, the render worker creates a DXGI flip-model swapchain and uploads processed BGRA8 CPU frames to a shader-resource texture for aspect-correct SDR display. Unbound Windows sessions and non-Windows builds still use headless presentation. Resource binding/replacement/detach remains render-thread-affine and acknowledged, while the host retains ownership of its HWND. The decode, process, scheduler, audio, and command boundaries are unchanged; GPU-native frame ingestion, HDR, and automatic device-loss recovery are follow-up modules.

On Windows the same composition root supplies `WasapiSharedAudioOutputProvider` and creates `FfmpegAudioFrameConverter` instances through the portable processor factory. Thus media with audio reaches the existing real shared-mode audio branch when playback starts; non-Windows audio output composition remains a future backend concern.

Video flows Video Decode -> Process -> Scheduler -> Render under the authoritative playback clock. The audio branch flows Audio Decode -> Process -> Render: `AudioDecodeWorker` produces Lapis-owned `AudioFrame` values, `FfmpegAudioFrameConverter` performs drainable libswresample conversion behind `IAudioFrameProcessor`, and `AudioRenderWorker` drives an `IAudioOutput` through bounded partial submission, stop-aware readiness waits, drain, reset, and synchronous render-thread-owned Pause/Resume control. Video-only and audio-only generations are both valid when the required consumers are configured.

Audio output target selection has its own portable `IAudioOutputProvider` boundary. A provider prepares an `AudioOutputPlan` for each playback generation, containing the exact `AudioFormat` that the converter should produce plus an unopened output already bound to the same physical/logical endpoint used for negotiation. Seek therefore naturally renegotiates the output target without allowing default-device changes to split discovery and rendering across endpoints.

A newly prepared audio generation starts Decode and Process immediately, but its processed stream remains bounded and pending until the authoritative clock is Running. Initial Start and running Seek start Audio Render only after the clock transition. A Seek performed while Paused may fill bounded decode/process queues, but the replacement endpoint is not opened until Resume. Pause freezes an active audio endpoint before pausing logical time; Resume makes logical time Running before resuming or starting hardware presentation.

`AudioRenderWorker::pause()` and `resume()` are synchronous control-plane acknowledgements rather than direct device calls. A dedicated wait-interrupt source wakes bounded input waits, device backpressure waits, or EOF drain; the render thread then applies `IAudioOutput::pause/resume` and acknowledges the control revision. A partial frame retains its sample cursor and EOF retains its drain phase across Pause.

On Windows, `WasapiSharedAudioOutputProvider` resolves the default multimedia render endpoint, reads its shared engine mix format with `GetMixFormat`, converts that descriptor into the Lapis audio model, verifies the reconstructed format with exact shared-mode `IsFormatSupported`, and returns a `WasapiSharedAudioOutput` bound to the captured endpoint ID. The output maps Pause to `IAudioClient::Stop` and Resume to `Start`, preserving queued endpoint samples and stream position; reset remains Stop + Reset for discontinuities.

`WasapiSharedAudioOutput` acquires `IAudioClock`. The first successfully released sample prefix maps stream position zero to its canonical media timestamp. Render-thread refreshes combine `IAudioClock::GetPosition` with `GetFrequency`, bridge the returned QPC correlation into the same monotonic domain used by `PlaybackClock`, and publish a coherent `AudioOutputPosition` snapshot through atomics. `position()` never calls COM and can be read by synchronization code without crossing device-thread affinity.

`AudioClockDiscipline` is the portable policy between that snapshot and the authoritative clock. It ignores non-hardware, stale, implausibly future, paused, and stopped observations; extrapolates a valid device sample to the caller's monotonic `now`; computes signed drift against the same-time `PlaybackClockSnapshot`; and suppresses corrections inside a configurable deadband.

`AudioClockSyncWorker` is the generation-scoped application layer. While playback is Running it samples at a configurable cadence (20 ms by default), evaluates the discipline decision, revalidates the output revision, and compare-applies only Rebase decisions through `PlaybackClock::try_rebase`. The compare step requires state, epoch, and revision to still match the snapshot used for evaluation, so Pause/Resume/Seek races reject stale corrections. When the logical clock is Paused or Stopped, the worker blocks on a clock-revision wait instead of polling. `PlaybackExecution` exposes synchronization diagnostics but keeps WASAPI and FFmpeg types outside this layer.

The audio hardware clock can therefore discipline the single authoritative `PlaybackClock` without becoming a second product clock. Corrections inside one playback epoch wake video timing only when drift actually exceeds policy, while media discontinuities still use the explicit epoch mechanism.

Blu-ray navigation, explicit endpoint selection/change notifications, fallback target-format policy for non-representable engine mix formats, long-term frequency/rate correction, additional hysteresis/rate limiting based on real-device measurements, external/video-master policies, `IAudioClient3` low-latency period policy, exclusive/bitstream output, GPU decode/processing, D3D11 zero-copy presentation and HDR output, full color/HDR metadata, subtitles, AI taps, FFmpeg interrupt callbacks for blocking sources, cross-platform audio output composition, and full Blu-ray navigation remain deliberately outside this slice.
