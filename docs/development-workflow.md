# Lapis development workflow

## Branching rules

The protected integration target is **`master`** (not `main`). Do not commit directly to it.

1. Bootstrap the existing M1d source on `chore/bootstrap-m1d-baseline`.
2. Validate the imported archive before marking the baseline PR ready.
3. Merge baseline only after review and test verification.
4. Branch `feat/m1e-native-ffmpeg` from the merged baseline, not from a speculative local snapshot.
5. Each subsequent feature should have an independently reviewable branch and PR.

Use one logical commit or a small number of meaningful commits per feature. Avoid commit-per-file and automated pushes. No forced updates to the default branch.

## Source-of-truth requirements

- GitHub is the source of truth **after** M1d import and merge. Until then, the M1d ZIP remains the local source snapshot.
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

## Baseline import

Download the M1d ZIP from the project conversation and run:

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

Never merge the draft baseline PR until this import and the required review are complete.
