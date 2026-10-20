# M1d GitHub import verification — 2026-10-20

## Integrity
- Source ZIP: `bdmv-player-v0.2-m1d.zip`
- ZIP SHA-256: `06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932`
- PR: #41, branch `chore/bootstrap-m1d-baseline`
- Verified Git commit containing the full archive content: `b23c2b7c8168371370ec11294840912048c7b780`
- Git tree: `0e2a7cf1253981eb8e221e06276aa6d038f09783`
- Original ZIP blobs: **67/67** found in remote recursive tree with **exact SHA matches**. Mismatches: **0**.
- Original `README.md` was preserved under `docs/historical/m1d-README.md`; original `.gitignore` under `docs/historical/m1d.gitignore`. The current repository's top-level policies are intentionally kept.
- Additional build/import workflow files exist alongside the 67 preserved blobs.

## Reproduced local Linux test results

| Compiler | Configuration | Results |
|---|---|---|
| GCC 14.2 | Release, `BDMV_WITH_FFMPEG=OFF`, tests ON, preview OFF | **10/10 passed** |
| GCC 14.2 | Release, `BDMV_WITH_FFMPEG=OFF`, SDL dummy preview ON | **11/11 passed** |
| Clang 17 | Debug, `BDMV_WITH_FFMPEG=OFF`, SDL dummy preview ON | **11/11 passed** |

Builds were executed from a **fresh unpack of the SHA-pinned ZIP**. The remote Git tree's original source files were verified to be byte-identical; it was not possible to run `git clone` directly inside this isolated container. Only the root README/.gitignore differ intentionally, with originals archived separately. Those policy files are not inputs to CMake.

## Not yet validated

- Native `BDMV_WITH_FFMPEG=ON` integration (FFmpeg developer packages unavailable here)
- Real MKV/MP4 presentation to a physical display or speaker and accurate A/V sync
- Windows/MSVC and managed .NET 10 executable
- SteamOS/Deck/Wayland/Gamescope/HDR and Blu-ray
- SDL2 dummy device tests are API tests, **not real hardware playback**.

## Actions cost policy

The imported `.github/workflows/manual-portability.yml` has only `workflow_dispatch` and no push/pull_request triggers. No GitHub Actions workflow was dispatched or run by this import.

## Recommended PR action

Review the complete Git diff, note the above unverified configurations, and only then merge PR #41 into `master`. M1e FFmpeg work can begin from the updated master after merge.
