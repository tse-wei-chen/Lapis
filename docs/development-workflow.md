# Lapis development workflow

## Branching rules

The protected integration target is **`master`** (not `main`). Do not commit directly to it.

1. M1d source and historical docs are imported on `chore/bootstrap-m1d-baseline` and verified by Git Blob SHA (67/67).
2. Review the verified GCC/Clang Linux test report and missing-platform limitations before merging baseline PR #41.
3. Merge baseline only after review; use `master` as the stable integration target.
4. Branch `feat/m1e-native-ffmpeg` from the merged baseline; no speculative partial snapshots.
5. Each subsequent feature should have an independently reviewable branch and PR.

Use one logical commit or a small number of meaningful commits per feature. Avoid commit-per-file and automated pushes. No forced updates to the default branch.

## Source-of-truth requirements

- The complete import branch is now the proposed source of truth; after approval and merge, `master` becomes authoritative. The M1d ZIP remains a SHA-pinned verification artifact.
- Do not represent SDL dummy-driver tests as real speaker or display playback.
- Do not represent FFmpeg CLI diagnostics as in-process FFmpeg Adapter verification.
- Record GCC/Clang, MSVC, FFmpeg-on, SDL dummy, and real-device test status independently.
- Maintain the portable MediaRuntime core without platform-specific graphics/audio APIs and do not leak `AV*` types through the public C ABI.

## Validate M1d on Linux

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBDMV_WITH_FFMPEG=OFF -DBDMV_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Enable the optional SDL preview only if your developer environment includes the SDL2 runtime:

```bash
cmake -S . -B build-preview -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBDMV_WITH_FFMPEG=OFF -DBDMV_BUILD_PREVIEW=ON -DBDMV_TEST_SDL_DUMMY=ON
cmake --build build-preview -j
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./build-preview/bdmv_sdl_preview test://frames --max-seconds 6
```

Real FFmpeg decoder integration requires the FFmpeg development libraries and an explicit `-DBDMV_WITH_FFMPEG=ON` build. Do not claim successful playback without it.

## GitHub Actions cost policy

Do **not** enable workflows with `push` or `pull_request` triggers during bootstrap. If a portability workflow is imported from the M1d archive, it must stay **`workflow_dispatch`-only**. Review test matrix cost before manual dispatch. Do not automatically rerun failed jobs without checking the cause.

## Historical baseline import (optional reproducibility)

All original ZIP contents are already committed to PR #41. The following procedure is retained only to reproduce the import on a fresh staging checkout:

```bash
git clone https://github.com/tse-wei-chen/Lapis.git
cd Lapis
git fetch origin chore/bootstrap-m1d-baseline
git switch --track origin/chore/bootstrap-m1d-baseline
bash scripts/import-m1d.sh /path/to/bdmv-player-v0.2-m1d.zip
git diff --check
git status --short
# Run local tests and review the diff, then explicitly commit/push once.
```

Do not merge without independent review and confirmation of the recorded test limitations.
