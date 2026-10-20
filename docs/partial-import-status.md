# M1d source import status

Archive: `bdmv-player-v0.2-m1d.zip` (67 files), SHA-256 `06355de16608252f79e7f4194d24a7192c4764e78db742c84335f3fb8ccd2932`.

## Transferred to GitHub
- The exact M1d core headers `decode_source.hpp`, `decoded_media.hpp`, `decode_pump.hpp`, `epoch_queue.hpp`, `media_time.hpp`, `platform_interfaces.hpp`, `playback_clock.hpp`, `presentation_timing.hpp`, `session.hpp` (**9/67**).
- Each transferred file's Git Blob SHA was compared to the original archive's Git Blob SHA; all nine matched.
- A separate header-only portable contract smoke test in `tests/imported-core-contracts.cpp`.
- Import helper safely skips pre-existing identical file bytes and refuses conflicting content.

## Reproduce the partial core check

```bash
bash scripts/test-imported-core.sh
```

The contract smoke was compiled/executed locally with GCC and Clang, with `-Wall -Wextra -Werror`. It validates key time conversion, epoch isolation, bounded RGBA calculations, clock Pause/Seek, video scheduling, and diagnostic queued-audio estimation.

## Not done
- **58/67 archive entries** are still not on GitHub. This is a deliberately incomplete Draft PR.
- The branch is not yet a buildable CMake application: the rest of M1d source is still required.
- No real FFmpeg in-process decode or physical speaker/display proof; Windows and SteamOS remain unverified.
- Do not mark PR #41 ready or merge until full sources, build and tests are in the branch.
