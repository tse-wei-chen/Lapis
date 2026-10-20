# M1d — SDL2 developer preview (2026-10-20)

## Outcome

A **separate opt-in SDL2 diagnostic executable** now consumes frames through the existing
`MediaRuntime` **C ABI 1.1**. The library remains SDL/GUI/OS independent.

- RGBA8 frames are uploaded to an SDL streaming texture and submitted to an SDL renderer.
- Interleaved 48 kHz S16LE stereo PCM is sent to `SDL_QueueAudio` with a 250 ms staging limit.
- A bounded video presentation queue holds up to 8 frames; the decoder pump already has
  bounded frame/byte queues. Playback can be paused/resumed and seeked by keyboard.
- The video scheduler makes deterministic early/due/drop decisions based on PTS and a
  steady-clock fallback timeline. Current lateness threshold is 80 ms.
- The preview reports a diagnostic SDL queued-audio playhead estimate. **This is NOT a
  calibrated audio-device presentation clock**; actual A/V sync, device latency and drift
  compensation are still M1 work.

The prototype uses a small SDL2 **runtime-loader shim** rather than copying SDL into
MediaRuntime. This avoids adding mandatory SDL SDK headers to the core build.
**It does not imply SDL2 is our final rendering, audio or input backend.** The eventual
Window/Surface and Audio Sink adapters will have their own ABI and platform tests.

## Run the synthetic presentation test

On a Linux machine with SDL2 runtime library installed:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBDMV_WITH_FFMPEG=OFF -DBDMV_BUILD_TESTS=ON \
  -DBDMV_BUILD_PREVIEW=ON -DBDMV_TEST_SDL_DUMMY=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/bdmv_sdl_preview test://frames
```

Headless (for CI/local validation):

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./build/bdmv_sdl_preview test://frames --max-seconds 6
```

Expected diagnostic result from this environment:

```
video_presented=8 video_dropped=0 queued_pcm_bytes=15360 eof=1
estimated_audio_playhead_ns=320000000
```

For real MP4/MKV **where FFmpeg development libraries and headers are installed**, build
with `-DBDMV_WITH_FFMPEG=ON` and invoke `./build/bdmv_sdl_preview /path/to/file.mp4`.
**This real-media path is source-complete only as an initial prototype and was NOT compiled
or verified in this authoring environment.**

Keyboard: `Space` pauses/resumes, `Right Arrow` seeks forward 5 seconds, `Q`/`Escape` quits.
The SDL runtime must be present when executing the preview; the core does not link it.

## Verified

- Linux GCC 14 Release: 11/11 CTests, including SDL2 dummy video+audio presenter.
- Linux Clang 17 Debug: 11/11 CTests, same coverage.
- Linux Clang 17 ASan+UBSan Debug: 11/11 CTests, same coverage.
- Linux Release without test sources: 8/8 CTests, avoids shipping fake source.
- SDL dummy test exercises `SDL_Init`, window+renderer+texture creation, texture upload,
  render presentation, audio device setup, PCM queuing and clean shutdown. It does not
  drive a physical display/speaker or verify perceived quality.

## Not done — explicit M1 acceptance gates

- Link and test **in-process** FFmpeg decoding with dev headers on Linux and Windows.
- Implement an actual hardware-audio-master clock and feedback/latency estimation.
- Decode correct timestamps and ordering across A/V streams, VFR, seek/discontinuities.
- Verify visual and audible output on physical devices, including multi-monitor changes.
- Complete A/V pause/drain/seek tests for real MP4/MKV, HEVC, 10-bit and variable fps.
- Migrate the preview to supported SDL2 SDK or own backend contracts before product use.
- Ship proper Unicode/local-URI handling, stream selection and error messaging.
- SteamOS/Steam Deck Gamescope, controller interaction and HDR remain untested.
