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
- Linux builds now choose an XCB **X11/XWayland CPU fallback presenter**.
  Without an explicitly bound host-owned X11 Window XID, it remains headless;
  a real X11 window receives BGRA video pixels. Linux now also has an **ALSA
  PCM output adapter** that can use PipeWire's ALSA compatibility plugin when
  installed. Full desktop hosting/Steam Deck acceptance remains outstanding.
  Other non-Windows platforms currently remain headless.
- `Lapis.MediaRuntime`: same public C ABI entry points on both builds;
  `media_runtime.cpp` delegates platform selection to the factory.
- The Windows Avalonia client still targets `net10.0-windows`, hosts an HWND,
  and uses XInput. This PR does not claim a Linux desktop app.

The Platform target now accepts an explicit `X11Window` surface descriptor
(a 32-bit XID carried in the existing pointer-width C ABI handle). The XCB
presenter owns a separate render-thread connection and **never owns the host
window**; detach releases the borrowed XID before acknowledging the call.
It uses the existing FFmpeg BGRA CPU fallback and reads back actual pixels
under Xvfb in CI. This is **not Vulkan or native Wayland**. Those require
separate presenters. A native PipeWire sink and a hardware-correlated playback clock
remain outstanding; ALSA playback is available using an explicit F32 48kHz
stereo target with no invented device timing.

## Reproduce the Ubuntu CI job

Ubuntu 24.04 or equivalent, with modern C++20 CMake and FFmpeg development
headers installed:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev libxcb1-dev libasound2-dev xvfb xauth

cmake -S native/MediaRuntime -B build/linux-headless \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DLAPIS_ENABLE_LIBBLURAY=OFF
cmake --build build/linux-headless --parallel 2
xvfb-run -a ctest --test-dir build/linux-headless --output-on-failure \
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

The `linux_headless_platform_boundary` test asserts that unbound Linux
sessions remain headless and Win32 HWNDs are rejected. The new
`x11_real_window_pixel_readback` test uses `xvfb-run` to create an actual
server-side X11 window, submit BGRA frames, read pixel colors back from
the X server, resize with aspect-preserving black bars and detach. A no-op
headless presenter cannot pass that test.

## Still required for Linux / Steam Deck playback

1. Replace/augment the real X11 CPU fallback with a **Vulkan presenter**,
   then validate native Wayland / Gamescope separately. A Vulkan GPU path is
   not yet implemented.
2. Implement a **native PipeWire** audio output (ALSA is now the real fallback),
   device clock correlation, and sample-buffer-safe pause support for PCM
   plugins without native pause capability.
3. Decouple the Avalonia desktop host from HWND, Windows overlays and XInput.
4. Validate GPU device recovery, controller focus, suspend/resume, audio clocks,
   HDR pipeline and deployment permissions on **real Steam Deck hardware**.
5. Later extend macOS through Cocoa/Metal/CoreAudio adapters.

Do not report Linux/SteamOS as a supported playback target until real
audio/video output and end-to-end tests pass there.

## ALSA PCM integration (new)

The Linux Platform composition root now enables the existing audio
Decode → FFmpeg Convert → AudioRenderWorker pipeline via
`AlsaPcmAudioOutputProvider`. The provider resolves the ALSA PCM `default`
endpoint (or optional `LAPIS_ALSA_PCM` override), verifies **F32 48 kHz
stereo/interleaved**, then uses real `snd_pcm_writei` partial-prefix writes,
bounded stop-aware waits, drain, reset and reopen. A PCM device without pause
support explicitly reports an error instead of pretending that it preserves
queued audio. Source files under `Lapis.MediaRuntime.Core` do not import ALSA
headers or link the library.

The Linux CI includes `alsa_null_pcm_audio_output` which exercises the real
libasound `null` PCM plugin, including format negotiation, actual accepted
samples, drain, reset, close, reopen and cancellation. This proves API/adapter
integration but **does not prove audible playback on a hardware endpoint**.
ALSA now publishes a thread-safe **Estimated** output position after real
PCM prefixes have been accepted. The estimate combines the canonical media
timestamp of the accepted prefix with `snd_pcm_delay` (queued sample frames)
and the local steady-clock sampling instant. It is invalidated on close,
open, reset, unrecoverable PCM errors and device pauses; revisions prevent
cached samples from crossing generations. Saturating conversions protect
malformed/overflowing timestamps, and Linux CTest verifies the estimated
timeline, partial-prefix accounting and reset/close invalidation.

This is **not a hardware timestamp**: the native clock-discipline service
correctly ignores `Estimated` observations rather than rebasing the main
playback clock from a potentially inaccurate ALSA plugin latency. True
hardware-correlated ALSA/PipeWire timestamps, jitter validation and Steam
Deck sound-card testing remain future milestones. On Linux, installing
PipeWire's ALSA adapter may route the system `default` PCM through PipeWire,
but that is **not** native PipeWire API integration.
