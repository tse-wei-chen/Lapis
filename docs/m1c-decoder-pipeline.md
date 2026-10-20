# M1c — Session decode worker / frame handoff

Date: 2026-10-20. **Implementation: partial M1c; not a working GUI/audio player.**

## Implemented

1. `core/decode_source.hpp`: `IMediaDecodeSource` plugin contract; all buffers owned, portable headers only.
2. `core/decode_pump.*`: dedicated worker thread, bounded mailbox (8 items and 128 MiB), backpressure, sequential `next()`, mutex-serialized `seek()`, generation fencing, end/error status and orderly join on close.
3. `core/session.*`: connects `test://frames` (diagnostic) and local file paths/`file:///` URIs (only when `BDMV_WITH_FFMPEG=ON`). State-only `test://synthetic` remains backward compatible.
4. `include/bdmv/player_c_api.h`: ABI 1.1 frame read API, explicit `AGAIN`, `BUFFER_TOO_SMALL`, `END_OF_STREAM`, `DECODE_ERROR` statuses; C# mirrored layouts in `src/Player.Contracts/NativeMethods.cs` (not compiled here).
5. `tools/bdmv_frame_dump.cpp`: offline PPM first-video-frame and S16LE PCM output, **through MediaRuntime's C ABI**, not an external FFmpeg subprocess.
6. FFmpeg Adapter is now linked into MediaRuntime when enabled, retains EAGAIN packets and resets post-seek state. It is still **uncompiled and unverified** here because FFmpeg development packages are missing.

## Verified builds

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBDMV_WITH_FFMPEG=OFF -DBDMV_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GCC 14 Release **9/9**. Clang 17 Debug **9/9**. `decoder_pipeline` tests a blocking decoder's in-flight seek race, two-call ABI buffer negotiation, generation fencing, EOF/reseek and close. `synthetic_frame_dump` verifies PPM dimensions and PCM size from the real MediaRuntime ABI.

Run offline diagnostic manually (after building with test source):

```bash
./build/tests/native/bdmv_frame_dump test://frames /tmp/m1c-test
# /tmp/m1c-test.ppm  (P6, 2x2 fake picture)
# /tmp/m1c-test.s16le (48000 Hz, 2ch, signed 16-bit LE raw PCM)
```

For real MP4/MKV **requires actual FFmpeg headers/libs and successful enabled build**:

```bash
# Ubuntu/Debian developer environment, with package installation permissions:
sudo apt-get install pkg-config libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev ffmpeg
cmake -S . -B build-av -G Ninja -DBDMV_WITH_FFMPEG=ON -DBDMV_BUILD_TESTS=ON
cmake --build build-av -j
ctest --test-dir build-av --output-on-failure
./build-av/tests/native/bdmv_frame_dump /absolute/path/to/video.mkv /tmp/video-diagnostic
```

The FFmpeg-on `ffmpeg_adapter_fixture` generates 1s MP4/MKV, decodes using both the adapter directly and the MediaRuntime ABI, and checks frame / PCM output. **That FFmpeg-on test has not been run here**.

## Known limitations before claiming M1 complete

- No **audio device sink**, native window / video presenter, display composition, AV clock/master synchronization, color metadata propagation, GPU decode or true Pause/Resume of presented time. `play()` currently permits prefetch of decoded frames; the dump tool drains as fast as possible, not in real time.
- No codec-level interruption, bounded close latency, timed playback, network streams, BD-J, Blu-ray, DRM, subtitles, AI, HDR, hardware acceleration.
- `read_frame()` returns decoded buffers only. Caller must allocate at least `required_bytes` and handle `AGAIN`; output memory remains caller-owned. Frame timestamps can be missing or negative for actual containers; the current prototype does not normalize all real-world stream timelines.
- Failures from real-file decoder open currently map to `UNSUPPORTED` without exposed diagnostic string (error contract / telemetry is future work).
- `%`-escaped file URI paths are intentionally rejected until a strict URL parser is available; passing a plain local path works in FFmpeg-enabled builds.
- .NET 10 and Windows / SteamOS hardware integrations have not been compiled/tested in this environment.

## Next cut (M1d)

1. Get FFmpeg-enabled Linux build and real-media fixture green, fix all actual compile/runtime failures.
2. Implement true output plane: SDL/native platform CPU presenter + device PCM sink **behind interfaces**, not in Core.
3. Introduce bounded presentation scheduler anchored to audio-hardware clock; measure drift under seek/pause/resume and 23.976/24/29.97/60 fps content.
4. Cancellation-aware decode and seek command actor; deterministic stop during slow/corrupt local media.
5. Add source errors, track metadata / duration, PTS/DTS normalization, delayed resampler EOF draining, channel layout mapping and HDR awareness before production claims.
