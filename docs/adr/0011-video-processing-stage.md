# ADR 0011: Video processing is a dedicated execution stage

- Status: Accepted
- Date: 2026-10-17

## Context

Decoded video frames currently retain the decoder's native pixel format, commonly planar YUV. A graphics presenter should not also own color/pixel conversion: doing so would couple rendering backends to decoder formats and force every future D3D/software presenter to duplicate conversion policy.

The future hardware path also needs room for a GPU-native processor without changing scheduling or rendering contracts.

## Decision

1. `IVideoFrameProcessor` is the third-party-independent video-processing contract.
2. Processing runs on its own `VideoProcessWorker` between Decode and Scheduler.
3. The processing worker consumes and emits epoch-scoped `VideoFrameStream` objects connected by bounded, stop-aware queues.
4. `PlaybackExecution` owns the processor and extends generation startup to Decode -> Process -> Scheduler -> Render.
5. Generation shutdown is the exact reverse: Render -> Scheduler -> Process -> Decode.
6. The first processor adapter, `FfmpegVideoFrameConverter`, uses libswscale and is isolated under `/process/ffmpeg/`.
7. The software converter accepts system-memory Gray/YUV/NV12/P010/RGB inputs supported by the core pixel-format enum and normalizes them to system-memory BGRA8 at the original dimensions.
8. Processor output preserves `PlaybackEpoch`, canonical frame timestamp, and source timestamp metadata.
9. Unsupported surface/pixel formats are explicit (`UnsupportedInput`) and fault the current video execution generation rather than silently passing an incompatible frame downstream.
10. Presenter adapters receive processed Lapis surfaces and do not perform general-purpose decoder pixel-format conversion.
11. Future GPU processing (D3D11 video processor, shader path, HDR/tone mapping) will implement the same processor contract or a compatible processing stage; scheduler and presenter lifecycle remain unchanged.

## Consequences

- D3D11 presentation can begin with a narrow BGRA8 system-memory upload path instead of absorbing YUV conversion policy.
- Decode, processing, scheduling, and rendering have separate backpressure boundaries and can be tuned independently.
- Paused playback can now backpressure through three bounded inter-stage queues; the existing downstream-to-upstream stop order remains mandatory and is tested with capacity-one queues.
- libswscale becomes an explicit FFmpeg feature and is recorded in `THIRD_PARTY.md`.
- The current converter uses FFmpeg/libswscale default color conversion behavior because `VideoFrame` does not yet carry full color primaries/transfer/matrix/range metadata. Accurate HDR/wide-gamut handling requires extending video color metadata before production HDR output is enabled.
