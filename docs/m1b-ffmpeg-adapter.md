# M1b — In-process FFmpeg software decode adapter (initial implementation)

Date: 2026-10-20. **Status: source implementation exists; FFmpeg-enabled target NOT compiled in this environment.**

## What changed

- `native/MediaRuntime/src/adapters/ffmpeg/{ffmpeg_decoder.hpp,ffmpeg_decoder.cpp}`:
  - local-file-only open through `libavformat`; streams/codecs selected with `av_find_best_stream` and `libavcodec`
  - `avcodec_send_packet` / `receive_frame` and EOF drain
  - RGBA8 conversion through `libswscale`; interleaved stereo S16/48kHz audio conversion through `libswresample`
  - seek through `avformat_seek_file`, codec flush and resampler reset
  - owns all FFmpeg resources, no FFmpeg pointer escapes to Core / C ABI
- `core/decoded_media.hpp`: owned RGBA/PCM buffer DTO, session/epoch stamp, frame-size bound checks.
- `tests/native/decoded_contracts.cpp`: epoch mismatch, reset invalidation and buffer-size bound checks.
- `tests/native/ffmpeg_decode.cpp`: **optional** FFmpeg-linking integration test; counts decoded frames/audio and repeats after seek.
- `tools/ffmpeg_adapter_fixture.py`: generates real MP4/MKV and invokes native integration executable. Requires headers/libs and CLI.

## Build with development dependencies

Debian/Ubuntu development environment (not installed automatically):

```bash
sudo apt-get install cmake ninja-build pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    ffmpeg python3
cmake -S . -B build-m1b -G Ninja -DBDMV_WITH_FFMPEG=ON -DBDMV_BUILD_TESTS=ON
cmake --build build-m1b -j
ctest --test-dir build-m1b --output-on-failure
```

Windows (MSYS2/MinGW or vcpkg plus `pkg-config` and matching shared FFmpeg dev packages):
- Supply compatible `pkg-config` package metadata (`.pc` files) for all five libav modules.
- CMake config has **not** yet been tested with MSVC, CRT boundaries, or DLL redistribution.

Without headers, keep the portable build:

```bash
cmake -S . -B build-noffmpeg -DBDMV_WITH_FFMPEG=OFF
cmake --build build-noffmpeg -j
ctest --test-dir build-noffmpeg --output-on-failure
```

## Not yet implemented (do not mislabel as playback)

The adapter is a **sequential decode prototype**, not an operational end-to-end player.
It is not wired into `bdmv_session_open_uri`, which still returns `BDMV_UNSUPPORTED` on real media files. No actual PCM device sink, display presenter, concurrent decode scheduler, command mailbox or audio-master sync exists. Real-world seek may deliver keyframe-preroll packets and frames earlier than requested PTS; callers must filter and timestamp-normalize. The adapter currently emits frames in decoder processing order, not a guaranteed A/V master-time presentation order. It does not flush delayed `libswresample` samples at EOF, which can omit a small audio tail. Color/HDR metadata, track switching, reconfiguration, discontinuity, corrupted file recovery, sampling/drift and cancellation still require design/verification. The RGBA8 path is inherently SDR-oriented and not a valid HDR presentation strategy.

## Proof boundaries

Actual local checks performed on Linux:

- GCC 14 Release `BDMV_WITH_FFMPEG=OFF`: **7/7 CTest passed**.
- Clang 17 Debug `BDMV_WITH_FFMPEG=OFF`: **7/7 CTest passed**.
- The pre-existing `ffmpeg_cli_diagnostic` exercises FFmpeg **as an external executable**, not this new adapter.
- Enabled build and native FFmpeg integration fixture: **NOT run** (no development headers or pkg-config FFmpeg modules available; apt registry inaccessible here).
- Windows, SteamOS, .NET, player presentation: **NOT run**.

## Next engineering tasks in recommended order

1. Establish FFmpeg-enabled Linux CI build / `ffmpeg_adapter_fixture.py` green and eliminate compile/runtime issues if any.
2. Add `IMediaDecodeSource` read/cancel/track descriptors and source timeline semantics (including missing/negative PTS) to runtime; no FFmpeg types on the contract.
3. Add dedicated decoder worker, bounded epoch-tagged output queues, and command actor for seek/close ordering.
4. Build a software CPU video presenter and PCM sink on each target OS; use audio hardware clock as main timeline, with drift/regression tests.
5. Only after true local A/V playback: hardware acceleration, GPU surface/capability negotiation, SteamOS Gamescope tests.
