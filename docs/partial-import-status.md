# M1d source import status — PR #41

Verified artifact: `bdmv-player-v0.2-m1d.zip`, SHA-256 `06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932`.

## GitHub source transfer
- **29 / 67 original ZIP files** now committed, all checked byte-for-byte using Git Blob SHA against the original archive.
- Includes native C++20 MediaRuntime, Session, DecodePump, FileSource, synthetic adapter, C ABI header/interop, CMake runtime build, P/Invoke `Player.Contracts`, managed smoke source, selected native contract tests, architecture boundary script and file probe source.
- Added two independent validation scripts not in the ZIP: `scripts/test-imported-core.sh` and `scripts/test-native-abi.sh`; neither claims real media playback.

## Reproduction from the current partial branch

```bash
bash scripts/test-imported-core.sh
bash scripts/test-native-abi.sh
python3 tests/architecture/test_boundaries.py
```

The native ABI script explicitly builds with `BDMV_BUILD_TESTS=OFF` and `BDMV_WITH_FFMPEG=OFF`, because **38 original entries are still missing**, including the native CTest CMake file and FFmpeg adapter. It then calls the compiled shared library from GCC and Clang test executables to read **16 finite synthetic video/audio blocks**, validates the C ABI, and checks Seek/Epoch generation.

## Verification evidence and limitations

- Locally reconstructed exact imported native source subset: **CMake/GCC Release built `libMediaRuntime.so` successfully**.
- Native ABI C++ smoke: **GCC and Clang both passed** (16 synthetic frames each).
- Architecture boundary audit: **passed** against the full local M1d ZIP.
- Full M1d ZIP was previously tested separately (10/10 core and 11/11 SDL dummy), but **these are not tests from a fresh GitHub checkout**.
- `.NET 10` SDK is unavailable in this Linux environment: managed source is SHA-verified but **not compiled here**.
- FFmpeg native adapter, real MKV/MP4 playback, actual video/audio output, Windows and SteamOS remain unverified.

## Merge gate

Do not merge Draft PR #41 until all original 67 entries are imported safely (README and .gitignore deliberately retained as repo policy, original README saved under historical docs), the full normal CMake + CTest matrix passes, manual-only Actions configuration is confirmed, and code review is complete.
