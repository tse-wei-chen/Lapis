# M1d GitHub baseline import — complete source transfer

Archive: `bdmv-player-v0.2-m1d.zip`; SHA-256: `06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932`.

All **67 original ZIP entry contents** are now in this branch, byte-for-byte verified by Git Blob SHA. Two entries intentionally live under historical paths to preserve the repository's Git workflow files:
- Original `README.md` → `docs/historical/m1d-README.md`.
- Original `.gitignore` → `docs/historical/m1d.gitignore`.

The remaining 65 ZIP paths match exactly, including the manual-only `.github/workflows/manual-portability.yml`. Additional branch setup files are kept (import script, PR template, independent smoke tests).

## Baseline verification — independently reproduced from verified ZIP

- Linux GCC 14.2 Release, FFmpeg disabled, default CMake/CTest: **10/10 passed**.
- Linux GCC 14.2 Release, SDL2 dummy-driver preview enabled: **11/11 passed** in an earlier local build.
- Native ABI smoke: GCC and Clang both passed on exact source subset (16 synthetic blocks).
- Full FFmpeg-on build not verified here: development headers missing.
- Windows/MSVC, SteamOS/Steam Deck, physical A/V output, .NET 10 executable not verified.

## Required before release claims

1. Rebuild the complete GitHub branch using the normal CMake configuration and run CTest.
2. Inspect the draft PR for accidental binary/generated files; check manual-only workflow.
3. Review code before merge. **Do not treat synthetic SDL dummy output as successful video playback.**
4. After accepted baseline merge, branch M1e from `master` and validate the real in-process FFmpeg adapter using MP4/MKV fixtures.

The archival report `docs/validation-report.md` records the earlier test snapshot and does not prove that SteamOS or physical devices were tested.
