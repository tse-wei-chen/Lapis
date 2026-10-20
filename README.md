# Lapis

Lapis is a cross-platform media player project targeting **Windows, Linux, SteamOS and Steam Deck**. Its long-term goal is BDMV/Blu-ray playback with optional non-blocking AI subtitles, transcription and translation.

## Repository state

The `master` branch is intentionally kept stable. Development takes place in short-lived feature branches.

**Current GitHub status:** The M1d baseline source has **completed GitHub import**: all 67 original archive entries are preserved byte-for-byte (the original README and .gitignore are stored under `docs/historical/`, so existing repository policies are not overwritten). The PR remains Draft pending final clean-branch verification and review. This is not evidence that playback is working.

The M1d prototype (currently available as a separate source archive in the project conversation) has a C++20 MediaRuntime, versioned C ABI, synthetic decoded-frame diagnostics, a selectable FFmpeg adapter, and optional SDL2 dummy-driver preview. **Real MP4/MKV playback on Windows/Linux/SteamOS is not yet validated.**

See [development workflow](docs/development-workflow.md). To import the M1d ZIP, run [the checked importer](scripts/import-m1d.sh) locally, review the diff, then commit once to the bootstrap branch.

## Development milestones

- **M1d baseline:** import and verify the original source archive (pending)
- **M1e:** compile FFmpeg adapter with dev headers and validate real MP4/MKV through MediaRuntime
- **M1f:** audio-device clock and real audio/video output integration
- **M2+:** GPU/HDR, BDMV navigation and SteamOS compatibility

No automatic Actions workflows are introduced by this bootstrap. Tests should run locally first.
