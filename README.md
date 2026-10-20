# Lapis

Lapis is a cross-platform media player project targeting **Windows, Linux, SteamOS and Steam Deck**. Its long-term goal is BDMV/Blu-ray playback with optional non-blocking AI subtitles, transcription and translation.

## Repository state

The `master` branch is intentionally kept stable. Development takes place in short-lived feature branches.

**Current GitHub status:** The M1d source bundle is **partially imported**: 19 byte-for-byte verified native M1d source files (nine Core headers plus ten build/runtime sources) are now on `chore/bootstrap-m1d-baseline`, but the remaining 48 original M1d entries (FFmpeg adapter, preview, tests, docs and tooling) have not been transferred. The PR remains Draft. This is not evidence that playback is working.

The M1d prototype (currently available as a separate source archive in the project conversation) has a C++20 MediaRuntime, versioned C ABI, synthetic decoded-frame diagnostics, a selectable FFmpeg adapter, and optional SDL2 dummy-driver preview. **Real MP4/MKV playback on Windows/Linux/SteamOS is not yet validated.**

See [development workflow](docs/development-workflow.md). To import the M1d ZIP, run [the checked importer](scripts/import-m1d.sh) locally, review the diff, then commit once to the bootstrap branch.

## Development milestones

- **M1d baseline:** import and verify the original source archive (pending)
- **M1e:** compile FFmpeg adapter with dev headers and validate real MP4/MKV through MediaRuntime
- **M1f:** audio-device clock and real audio/video output integration
- **M2+:** GPU/HDR, BDMV navigation and SteamOS compatibility

No automatic Actions workflows are introduced by this bootstrap. Tests should run locally first.
