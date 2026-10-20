# M1d changes (2026-10-20)

- Added portable, overflow-safe `VideoTiming` late/drop/wait policy and queued PCM
  playhead estimate (`src/core/presentation_timing.hpp`).
- Added `bdmv_sdl_preview` stand-alone developer-only SDL2 video/audio consumer;
  dynamically loads SDL2 runtime and uses the public MediaRuntime C ABI 1.1.
- Added bounded SDL queue, texture recreation on frame-size change, keyboard
  pause/resume/seek/quit and deterministic headless dummy-driver check.
- Added build flags `BDMV_BUILD_PREVIEW` and `BDMV_TEST_SDL_DUMMY`, both OFF by default.
- C-only ABI consumer still compiles as C, with C++ linker driver for sanitizer linkage.
- Added `presentation_timing`, `sdl_dummy_presentation` tests; 11/11 GCC/Clang/ASan tests
  pass in tested Linux runtime, test source disabled 8/8.
- Added ADR-0010 and M1d validation/runbook. FFmpeg-on, hardware output and A/V
  synchronization are **still not verified/complete**.
