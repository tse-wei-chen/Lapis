# Third-party manifest

Native dependencies are intentionally explicit and pinned.

## FFmpeg

- Version: 9.0.2 (`Lei`)
- Source: https://ffmpeg.org/
- Package source: Microsoft vcpkg, builtin baseline `a5f736224e85b32e25a8ffc52562333180396642`
- Enabled vcpkg features: `avcodec`, `avformat`, `swresample`, `swscale`
- Disabled by policy: `gpl`, `nonfree`, `all-gpl`, `all-nonfree`
- Usage boundary: native FFmpeg adapters only; FFmpeg types must not enter managed contracts or native core contracts
- Current uses: demux, software decode, software video pixel-format conversion, and software audio sample-format/sample-rate/channel-layout conversion through isolated adapters
- Redistribution posture: the selected configuration intentionally avoids GPL/nonfree vcpkg features; any future feature change must be reviewed here before merge

Every future native dependency must be recorded here with component/version, source, license/build configuration, enabled features, and redistribution impact.
