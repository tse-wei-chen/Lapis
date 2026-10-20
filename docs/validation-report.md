# Validation report — 2026-10-20 — M0 through M1d

Environment: Debian/Linux x86_64 container. **Not Windows, SteamOS or Steam Deck.**

## Verified in this environment

| Build | Configuration | CTest | Notes |
| --- | --- | --- | --- |
| GCC 14.2 | Release; FFmpeg OFF; preview + SDL dummy ON | **11/11 pass** | C ABI, synthetic decoder, output API, pure policy |
| Clang 17 | Debug; FFmpeg OFF; preview + SDL dummy ON | **11/11 pass** | Independent compiler |
| Clang 17 | Debug; AddressSanitizer + UndefinedBehaviorSanitizer; SDL dummy ON | **11/11 pass** | No sanitizer findings in exercised paths |
| GCC 14.2 | Release; test source OFF; preview OFF | **8/8 pass** | Core builds without fake input |

Specifically:
- Native `MediaRuntime` builds without optional FFmpeg; C ABI unchanged (major 1/minor 1).
- Decoder pipeline epoch race and caller buffer-size tests still pass.
- SDL runtime smoke consumes synthetic RGBA/PCM through actual `MediaRuntime` C ABI,
  initializes dummy video/audio devices, uploads/renders 8 video frames and queues
  8 PCM blocks (15360 bytes total). Output observed:
  `video_presented=8 video_dropped=0 queued_pcm_bytes=15360 eof=1`.
- `presentation_timing` checks PTS wait/present/drop decisions and arithmetic safety.
- FFmpeg CLI diagnostic separately decodes MP4/MKV through external FFmpeg, still
  **not** proof of native FFmpeg Adapter integration.

## Not verified

- FFmpeg development headers / pkg-config modules are absent. `BDMV_WITH_FFMPEG=ON` target, real media `bdmv_frame_dump`, integrated `ffmpeg_adapter_fixture` **not compiled/run here**. No actual MKV/MP4 decoder support can be claimed as tested.
- .NET SDK 10 is absent. `NativeMethods.cs` has been updated but is uncompiled here.
- Windows MSVC and SteamOS/Steam Deck builds, Flatpak sandbox, Wayland/Gamescope and hardware output **not tested**.
- A prototype SDL presenter and SDL audio queue are exercised **only under dummy drivers**. Physical screen/speakers are NOT tested; actual audio-master A/V synchronization, precise device latency and drift control are NOT implemented.
- libbluray, BD-J, Blu-ray menus, decryption, HDR, subtitles, AI translation and real device support **not implemented**.

## Risk / follow-up

Real hardware and FFmpeg-enabled build gates must be green before marking M1 as complete. The new steady-clock preview is not a calibrated hardware-audio-master pipeline; do not call `bdmv_session_play()` a playable release feature. Blocking decoder calls still have no cancellation mechanism.
