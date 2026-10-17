# ADR 0027: Public C ABI delegates transport to PlaybackExecution

- Status: Accepted
- Date: 2026-10-18

## Context

The native execution layer now owns the complete bounded video generation, optional audio generation, authoritative playback clock, device Pause/Resume semantics, and hardware-audio clock discipline. The exported C ABI still maintained a second transport model, however: Play and Pause only changed `lapis_transport_state`, while Seek called `PlaybackMediaSession::seek` directly. A managed caller could therefore observe `PLAYING` even though no decode, scheduling, render, or audio worker had started.

That split ownership also made future surface binding, device recovery, natural Ended state, and AI taps unsafe. Any feature wired through `PlaybackExecution` would behave differently depending on whether playback was driven through tests/internal code or through the actual public API.

One epoch semantic also becomes visible at this boundary. The public API advances epoch for Seek but not for the subsequent Play. A Seek performed while transport is READY therefore prepares a stopped timeline at the new epoch, and the next Play must start that exact prepared timeline. Requiring another epoch increment merely to start workers would incorrectly classify presentation start as a second discontinuity.

The current C ABI has no native-window, swap-chain, or surface-binding contract yet. Wiring it to execution therefore also needs an explicit temporary video sink without prematurely leaking D3D/DXGI types into the stable ABI.

## Decision

1. Each public `lapis_session` owns a composition-level pipeline containing a `PlaybackMediaSession` and a `PlaybackExecution`. The execution object is destroyed before its referenced media session.
2. Public Play, Pause, Seek, and Close delegate to `PlaybackExecution`. C ABI code no longer calls `PlaybackMediaSession::seek` directly and no longer treats transport enum mutation as playback.
3. Result-returning transport commands are serialized by a per-session command mutex. Snapshot reads retain a separate short-lived state mutex, so long worker joins or media operations do not require holding the published-state lock.
4. Open continues to be an epoch-changing source replacement. It retires the previous pipeline outside the state mutex, opens the new media session, composes a new execution owner, and only then publishes READY.
5. READY Play calls `PlaybackExecution::start(current_epoch, stored_position)`. PAUSED Play calls `resume()`. Pause calls `pause()`. Seek advances the public epoch, temporarily publishes SEEKING, and delegates the discontinuity to `PlaybackExecution::seek`. Close delegates to `PlaybackExecution::close` before retiring the pipeline.
6. `PlaybackExecution` records when a Seek occurred while it was Stopped. The exact prepared epoch + position may subsequently be used by Start once. Older epochs, a different position at the same epoch, and arbitrary same-epoch restarts remain rejected.
7. C ABI snapshots use `PlaybackExecutionSnapshot` for active clock position and effective Running/Paused/Ended/Faulted state. Natural Ended/Faulted observations are latched into the public transport state so later command validation sees the published state.
8. The composition root uses `FfmpegVideoFrameConverter` plus an explicit headless `IVideoPresenter`. The headless presenter consumes scheduled frames only because the ABI does not yet expose a video surface. It is an adapter at the composition edge, not a core rendering policy.
9. On Windows the composition root also injects `WasapiSharedAudioOutputProvider` and an `IAudioFrameProcessor` factory that constructs `FfmpegAudioFrameConverter` for the negotiated target format. Platform types remain outside `PlaybackExecution` and the C header.
10. No new D3D, HWND, DXGI, FFmpeg, WASAPI, worker, or clock types are added to ABI version 1. A future surface-binding API can replace the headless presenter without changing transport ownership.

## Consequences

- `LAPIS_TRANSPORT_PLAYING` now means a real `PlaybackExecution` generation was started/resumed rather than only a state variable being changed.
- Public Seek now uses the same shutdown order, epoch filtering, paused-seek behavior, audio renegotiation, and A/V clock discipline as internal execution tests.
- The stable C ABI and internal execution state can no longer drift independently during normal transport commands.
- A READY -> Seek -> Play sequence uses one discontinuity epoch and is covered by the C ABI smoke test.
- Windows public playback can reach the real shared-mode audio adapter when media contains audio.
- Video is still intentionally headless at the public boundary until the presentation-surface contract is designed; this PR does not pretend that a discard sink is the final desktop renderer.
- Commands are serialized rather than racing media mutations. Concurrent callers remain supported, but their mutations have one deterministic execution owner.

## Deferred

- A stable video-surface/presenter binding contract and concrete D3D11/DXGI presenter.
- Window resize, DPI, fullscreen, swap-chain lifetime, HDR/color-space negotiation, and GPU device recovery.
- Cross-platform audio output composition.
- Public diagnostic fields for detailed worker/audio-clock state beyond transport + position.
- Explicit public policy for replay/seek after natural Ended.
- Device loss/hot-switch recovery and user-selected audio endpoints.
