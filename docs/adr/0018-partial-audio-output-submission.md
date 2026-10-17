# ADR 0018: Audio output submission is sample-frame slice aware

- Status: Accepted
- Date: 2026-10-17

## Context

The first audio-output contract treated a decoded `AudioFrame` as an atomic submission unit. Real output devices expose a finite ring buffer whose currently writable capacity is unrelated to decoder frame boundaries. A decoded frame can be larger than the entire device buffer or larger than the capacity available at a particular wakeup. Requiring all-or-nothing frame submission would make such a frame permanently backpressured.

This must be fixed before a concrete WASAPI implementation, otherwise the Windows adapter would need hidden staging buffers or blocking writes that violate the output boundary.

## Decision

1. `IAudioOutput::try_submit` receives `first_sample_frame`, identifying the first not-yet-submitted sample in the immutable `AudioFrame`.
2. The call returns `AudioOutputSubmitResult { status, accepted_sample_frames }`.
3. `Accepted` always consumes a positive prefix no larger than the remaining samples. If the prefix is partial, the adapter has consumed all capacity currently available and the caller performs the existing stop-aware writable wait before retrying the suffix.
4. `Backpressure`, device errors, and format errors consume zero samples. Any implementation violating these progress rules is treated as a fault by `AudioRenderWorker`, preventing infinite loops or silent sample duplication/loss.
5. `AudioRenderWorker` owns the per-frame cursor. `submitted_sample_frames` advances for each accepted prefix, while `submitted_frames` advances only after the entire decoded/processed frame has been accepted.
6. `audio_plane_slice` centralizes overflow-safe pointer/length arithmetic for planar and interleaved sample-frame ranges. Output adapters call it only after the frame buffer has passed structural validation.
7. Output adapters still do not resample, remap channels, or reinterpret packing. Partial submission is transport pacing, not audio processing.

## Consequences

- A device buffer smaller than a decoded frame can make bounded forward progress without an unbounded staging queue.
- WASAPI can copy exactly the writable prefix into `IAudioRenderClient` memory and wait on its event for the remainder.
- Frame lifetime remains simple: the render worker holds the current frame until every sample is accepted or the generation is interrupted.
- Fake-output integration coverage exercises a 480-sample decoded frame through 128-sample chunks and verifies nonzero submission offsets.

## Deferred

- Concrete WASAPI shared-mode output and default-device capability discovery.
- Processor target-format selection from device mix format.
- PlaybackExecution ownership of the full audio generation and A/V clock discipline.
