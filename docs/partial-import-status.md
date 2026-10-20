# M1d source import status

Source archive: `bdmv-player-v0.2-m1d.zip`, SHA-256 `06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932`. It has **67 archive entries**.

## GitHub branch status: partial but buildable core

**19/67 original M1d entries** are now committed: nine Core headers, C ABI header, C ABI interop, Session and DecodePump implementations, file and synthetic adapters, root CMakeLists and MediaRuntime CMakeLists. Each copied file was checked against the original ZIP using Git Blob SHA.

Also included are the repo's independent portable Core smoke and a new native runtime/C ABI synthetic smoke.

To reproduce the *currently imported subset* without the original M1d test suite:

```bash
bash scripts/test-native-abi.sh
bash scripts/test-imported-core.sh
```

The first command explicitly sets `BDMV_BUILD_TESTS=OFF` because the original `tests/native` CMake sources have not been imported. It builds the shared `MediaRuntime` with `BDMV_WITH_FFMPEG=OFF`, then compiles/runs a native C ABI smoke against 16 diagnostic (not real-media) frames using GCC and Clang.

The **same exact source subset**, reconstructed locally from ZIP files, built successfully as `libMediaRuntime.so` using CMake/GCC; both GCC and Clang C ABI smoke checks passed. A *fresh remote branch checkout* has not been built directly in this environment (container GitHub network access is unavailable).

## Still missing from the original ZIP

**48/67 original entries**: FFmpeg adapter, SDL preview and utilities, architecture docs and ADRs, original tests and CTest registration, .NET sample. Do not run the default test-enabled CMake configuration until the test sources are imported. Local full ZIP tests from earlier sessions do not imply this partial GitHub checkout has those tests.

Do not merge PR #41 before all archive files are safely imported, the default CMake configuration builds, original CTest matrix passes, and manual-only Actions configuration is reviewed. FFmpeg-on, real video/audio output, Windows and SteamOS remain unverified.
