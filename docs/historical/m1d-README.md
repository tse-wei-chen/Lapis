# BDMV Player — Cross-platform v0.2 / M1d SDL2 output experiment

> **Prototype: genuine SDL2 video/audio API calls from synthetic MediaRuntime frames; NOT yet verified playback of real media or real display/speakers.**
>
> Windows, Linux, SteamOS and Steam Deck remain targets; only Linux builds without FFmpeg developer headers have been tested in this authoring environment.

## Current status (2026-10-20)

- C++20 `MediaRuntime` + versioned **C ABI 1.1** + optional .NET 10 P/Invoke source.
- Portable Session / Epoch / timestamps / PlaybackClock / FileSource from M0–M1a.
- `IMediaDecodeSource` + bounded, blocking decode worker + epoch-gated Seek + caller-owned RGBA8/PCM buffers.
- Diagnostic `test://frames` emits 8 fake RGBA (2x2) and 8 PCM (48k stereo) buffers; decoded bytes flow **through MediaRuntime**, not the FFmpeg CLI. Offline frame dump emits `.ppm` and `.s16le` for automated tests.
- Optional in-process FFmpeg local MP4/MKV decode adapter exists and is now linked into `MediaRuntime` when `BDMV_WITH_FFMPEG=ON`; **not compiled or real-file tested in this environment** due missing FFmpeg development libraries.
- Opt-in `bdmv_sdl_preview` consumes **Session C ABI frames** to create/update SDL2 textures and queue PCM; tested with SDL2 dummy video/audio drivers, not real hardware.
- Pure video scheduling policy decides present/wait/late-drop from external clock values; preview currently uses a steady clock, **not** an audio hardware presentation clock.
- No production surface, calibrated audio sink, actual device A/V sync, protected Blu-ray or BD-J.

## Portable build (FFmpeg disabled)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBDMV_WITH_FFMPEG=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/tests/native/bdmv_frame_dump test://frames /tmp/m1c-test
```

GCC 14 Release, Clang 17 Debug and Clang 17 ASan+UBSan Debug **11/11** tests passed with optional SDL dummy preview enabled; without synthetic sources **8/8** passed. The `.ppm` is a diagnostic synthetic image and `.s16le` raw PCM is not connected to speakers.

## Optional SDL2 developer preview (dummy-driver validated)

```bash
cmake -S . -B build-preview -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBDMV_WITH_FFMPEG=OFF -DBDMV_BUILD_PREVIEW=ON -DBDMV_TEST_SDL_DUMMY=ON
cmake --build build-preview -j
ctest --test-dir build-preview --output-on-failure
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./build-preview/bdmv_sdl_preview test://frames --max-seconds 6
```

SDL2 **runtime** library is required only for the optional developer preview executable.
Regular `MediaRuntime` has no SDL build or runtime dependency. See
`docs/m1d-sdl-preview.md` for output, limits and keyboard controls.

## Real-file developer build (NOT verified here)

```bash
sudo apt-get install pkg-config libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev ffmpeg
cmake -S . -B build-av -G Ninja -DBDMV_WITH_FFMPEG=ON -DBDMV_BUILD_TESTS=ON
cmake --build build-av -j
ctest --test-dir build-av --output-on-failure
./build-av/tests/native/bdmv_frame_dump /path/to/test.mp4 /tmp/media-diagnostic
# For preview only, add -DBDMV_BUILD_PREVIEW=ON when configuring, then:
# ./build-av/bdmv_sdl_preview /path/to/test.mp4
```

FFmpeg-on CTest's fixture generates temporary MP4/MKV and checks both direct FFmpeg Adapter and Session ABI extraction. Because this path was not compiled locally, **do not use this README as evidence of actual media compatibility**.

## Architecture documents

- `docs/architecture/architecture-v0.2.md` + `docs/architecture/architecture-v0.1-original.md`
- `docs/adr/0001-*` through `0010-*` — decisions, including bounded decoder worker / output boundary.
- `docs/m1d-sdl-preview.md` — tested SDL2 preview, commands and remaining gaps.
- `docs/m1c-decoder-pipeline.md` — earlier decode handoff changes and test boundaries.
- `docs/m1b-ffmpeg-adapter.md` and `docs/m1-foundations.md` — historical milestones.
- `docs/validation-report.md` — cumulative evidence and unverified platforms.
- `docs/steam-os-acceptance.md` — required device matrix.
- `docs/implementation-plan.md` — forward tasks.

## Managed build (not tested here)

`src/Player.Contracts/NativeMethods.cs` contains matching ABI definitions. A `.NET 10` SDK is required for `dotnet build src/Player.Smoke/Player.Smoke.csproj`; the SDK is not installed in this environment.

## Platform constraints

No FFmpeg, Blu-ray, Java or GPU libraries are vendored in this repository. Future binary distributions require licensing / SBOM review. All optional CI runs are manual (`workflow_dispatch`) to avoid unwanted GitHub Actions usage. No code has been pushed to GitHub.
