# M1d baseline verification — 2026-10-20

## Artifact identity

- Archive: `bdmv-player-v0.2-m1d.zip`
- SHA-256: `06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932`
- Source entries: **67**. Archive root: `bdmv-player-v0.2/`.
- The archive has **not yet been committed** to the GitHub bootstrap branch; these tests were run using the archive in a temporary Linux working directory.

## Locally reproduced checks

| Build/verification | Outcome |
|---|---|
| Linux GNU C/C++ 14.2 Release, `BDMV_WITH_FFMPEG=OFF`, core tests | **10/10 passed** |
| Linux GNU C/C++ 14.2 Release, `BDMV_BUILD_PREVIEW=ON`, `BDMV_TEST_SDL_DUMMY=ON`, dummy audio/video | **11/11 passed** |
| Import script smoke test into a clean test Git branch, then CMake + CTest | **10/10 passed** |
| ZIP SHA pin and count | **Matched (67 entries)** |
| Existing repository README / .gitignore after import | **Preserved** |

### Important exclusions

This does **not** prove the real FFmpeg Adapter was compiled, in-process MKV/MP4 playback works, or that output reached a real display or audio device. FFmpeg development headers, Windows, SteamOS, and managed .NET tests were unavailable in this runner. The SDL test exercises a dummy backend only.

## Import completion gate

1. Import the pinned archive into `chore/bootstrap-m1d-baseline` using `bash scripts/import-m1d.sh path/to/bdmv-player-v0.2-m1d.zip`.
2. Inspect the diff. Run native tests and validate the manual-only GitHub Actions trigger.
3. Commit the full source in **one logical commit** and push once.
4. Only then mark Draft PR #41 ready, review, and merge.
5. Branch M1e from the merged `master`. Do not create M1e from a docs-only baseline.
