# Linux headless portability gate (PR #69)

Status: **CI baseline / not Linux desktop playback**

This is the first executable step toward the accepted `architecture-v0.2`
cross-platform direction: Windows 10/11, desktop Linux, SteamOS/Steam Deck,
and a future macOS adapter. It checks source portability, not monitor output,
speaker output, GPU interop, or Steam Deck compatibility.

## Code boundaries

- `Lapis.MediaRuntime.Core`: source, demux, FFmpeg decoding/processing,
  subtitle timelines, bounded workers, scheduler, clock, graph and playback
  execution. No Windows-specific D3D11 or WASAPI source files linked here.
- `Lapis.MediaRuntime.Platform`: platform composition root, implemented in
  `src/platform/playback_execution_factory.cpp`. It chooses the existing
  D3D11 presenter + WASAPI sink **on Windows**.
- Non-Windows builds choose an explicitly **headless** presenter and no audio
  device sink. This is useful for testing native logic and the C ABI, but
  **does not display video or play sound**.
- `Lapis.MediaRuntime`: same public C ABI entry points on both builds;
  `media_runtime.cpp` delegates platform selection to the factory.
- The Windows Avalonia client still targets `net10.0-windows`, hosts an HWND,
  and uses XInput. This PR does not claim a Linux desktop app.

The separate Platform target is the future insertion point for Vulkan
presentation, PipeWire/ALSA audio output, and a Linux window-surface descriptor.
The existing Win32-only C ABI output surface is not misrepresented as X11 or
Wayland; extending it is an explicit follow-up change. Since Lapis has not
shipped, that change can be breaking without compatibility wrappers.

## Reproduce the Ubuntu CI job

Ubuntu 24.04 or equivalent, with modern C++20 CMake and FFmpeg development
headers installed:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev

cmake -S native/MediaRuntime -B build/linux-headless \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DLAPIS_ENABLE_LIBBLURAY=OFF
cmake --build build/linux-headless --parallel 2
ctest --test-dir build/linux-headless --output-on-failure \
    --parallel 2 --timeout 90
```

CMake first accepts `FFMPEG` supplied by the existing pinned vcpkg setup.
If no CMake FFmpeg package is available, it resolves the five FFmpeg libraries
through system `pkg-config`. The Linux job uses distribution packages and
does not build a second full FFmpeg dependency tree for every pull request.

## CI contract

`linux_headless` is required by the workflow for `native/*` changes and
changes to the workflow itself. It builds the shared C ABI runtime, portable
core, platform factory and native tests; then runs **CTest**. Windows still
runs its separate Native D3D11/WASAPI and actual desktop UI E2E acceptance,
alongside Managed tests. Linux deliberately does not execute Win32-specific
D3D11/HWND/WASAPI tests.

The `linux_headless_platform_boundary` test asserts that an unattached
headless presenter is usable and that a Win32 surface is **rejected**, rather
than presenting an empty Linux viewport as a successful GPU output.

## Still required for Linux / Steam Deck playback

1. Introduce a genuine Linux video surface and Vulkan presenter, with X11
   baseline and Wayland / Gamescope separately verified.
2. Implement PipeWire audio output and ALSA fallback behind the audio provider.
3. Decouple the Avalonia desktop host from HWND, Windows overlays and XInput.
4. Validate GPU device recovery, controller focus, suspend/resume, audio clocks,
   HDR pipeline and deployment permissions on **real Steam Deck hardware**.
5. Later extend macOS through Cocoa/Metal/CoreAudio adapters.

Do not report Linux/SteamOS as a supported playback target until real
audio/video output and end-to-end tests pass there.
