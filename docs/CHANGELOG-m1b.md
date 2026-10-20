# M1a → M1b change record (2026-10-20)

No GitHub commit/push or paid GitHub Actions run was performed.

- New: FFmpeg optional in-process decoder implementation, strictly adapter-scoped.
- New: owned RGBA/PCM frame contract, size caps and epoch verification test.
- New: optional native MP4/MKV decode + seek test (requires FFmpeg dev packages).
- New: CMake `BDMV_WITH_FFMPEG=ON|OFF` (default OFF); no mandatory new third-party dependencies.
- New: detailed build/validation instructions.
- Baseline: Linux GCC/Clang OFF-mode CTest 7/7 green. ON-mode NOT verified due to missing pkg-config FFmpeg development modules.
- No changes to exported C ABI or to MediaRuntime's still-synthetic session behavior.
