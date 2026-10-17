# ADR 0021: Audio pause is render-thread control, not a discontinuity

- Status: Accepted
- Date: 2026-10-17

## Context

The audio execution branch now has target-format negotiation and a concrete event-driven WASAPI output. Before it can be owned by `PlaybackExecution`, Pause must have semantics that preserve queued audio and synchronize with the device without allowing control-plane threads to call native device APIs directly.

The render worker can be blocked in three places when Pause arrives: waiting for the next bounded input frame, waiting for endpoint writability between partial sample-frame prefixes, or waiting for the endpoint to drain after EOF. A pause mechanism that only changes an atomic flag cannot wake those waits. Reusing generation stop would wake them, but would also make Pause destructive and lose the current frame cursor or drain phase.

WASAPI `IAudioClient::Stop` freezes the stream and its audio clock at the current position; a later `Start` resumes from that position. `Reset`, in contrast, flushes pending data and resets stream position. These map naturally to Pause/Resume versus seek/close discontinuities.

## Decision

1. `IAudioOutput` gains explicit `pause()` and `resume()` control operations returning `AudioOutputControlStatus`. They preserve queued samples and stream position. `reset()` remains the destructive discontinuity operation.
2. Only `AudioRenderWorker` invokes output pause/resume. Public worker control methods communicate desired state to the render thread and synchronously wait until that thread applies or rejects the command.
3. The render worker maintains a control revision and a replaceable `std::stop_source` used only to interrupt the current blocking queue/device wait. Generation shutdown still uses the `std::jthread` stop token.
4. A control interrupt returning `Stopped` from a queue/output wait is not itself terminal. The worker first checks generation stop, then applies pending control and resumes the exact interrupted phase.
5. The wait-interrupt source is renewed only after the render thread has observed and acknowledged the corresponding command. A caller may stop the source in the race between the worker's command check and its next blocking wait; that stopped token must reach the wait rather than being silently replaced.
6. Pause while a partial `AudioFrame` is in progress preserves `first_sample_frame`. Resume continues with the unsent suffix; already accepted prefixes are never duplicated.
7. Pause during EOF drain preserves the drain phase. Resume continues waiting for endpoint padding to reach zero rather than returning to the closed input stream.
8. Pause before the device stream has started is valid. The worker may enter logical `Paused` without invoking output control until an output has actually been opened.
9. WASAPI maps Pause to `IAudioClient::Stop` and Resume to `IAudioClient::Start`. Pending endpoint data is retained. Seek/close-style worker stop still performs output `reset()` before close.
10. If the first WASAPI `Start` fails after a prefix was copied and released to the endpoint, the adapter immediately stops/resets the stream before returning a zero-consumption error so the partial-submission contract remains truthful.

## Consequences

- `PlaybackExecution::pause()` can eventually wait for the audio device to be frozen before committing the logical playback-clock transition.
- Control-plane threads never need COM apartment affinity or direct access to WASAPI objects.
- Pause can wake bounded queue waits, device backpressure waits, and EOF drain without polling.
- The current sample cursor and endpoint queue survive Pause/Resume, while seek/close remain clean discontinuities.
- Worker state now distinguishes `Paused` from terminal `Stopped`.

## Deferred

- Wiring the audio branch and these pause/resume acknowledgements into `PlaybackExecution`.
- `IAudioClock` sampling, hardware-clock discipline, and A/V master-clock policy.
- Device recovery and renegotiation after loss while paused or resuming.
- Volume/mute control and user-visible output-device switching.
