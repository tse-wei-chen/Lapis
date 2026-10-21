# WebVTT sidecar support — v1

The existing provider-neutral timed-text picker can now select **local WebVTT
sidecars**, alongside local SRT, without modifying the native playback core or
the Watch UI. The new source ID is `local-webvtt`.

## Naming and selection

- `movie.vtt` and `movie.zh-TW.vtt` are discoverable beside `movie.mp4`.
- Invalid extra filename segments (e.g. `movie.zh_TW.vtt`,
  `movie.zh-TW.backup.vtt`), other movies and other file extensions are excluded.
- The Watch picker shows WebVTT as an explicit source. Its **Auto** policy
  intentionally remains the original SRT-first behavior; WebVTT does not
  silently override a user's existing SRT setup.
- The catalog supports two distinct subtitle sources without treating the
  subtitle file path as a universal track ID.
- WebVTT files are limited to 2 MiB; discovery is capped at 32 WebVTT files
  per movie (plus existing SRT limit); parser at 5,000 cues and five text lines
  per cue, 500 displayed characters.

## Supported parser subset

- UTF-8 with optional BOM; `WEBVTT` header and blank line
- WebVTT `mm:ss.mmm` and `hh:mm:ss.mmm` timestamps
- Optional identifiers and cue settings following the end time
- Comments (`NOTE`) and non-cue blocks (`STYLE`, `REGION`) skipped
- HTML entities decoded, inline tags stripped for safe plain-text display
- End-exclusive timestamp queries and nested overlaps with fallback to an
  earlier still-active cue, reusing the existing SRT timeline index

### Not supported

- Cue/region placement and CSS styling, voice/ruby rendering, karaoke timing,
  arbitrary WebVTT extension blocks, live segmented WebVTT, embedded FFmpeg
  subtitle streams, Blu-ray PGS/BD-J bitmaps or AI generated subtitles.
- OS file picker permissions and symbolic-link policy are not provided by this
  adapter; those require separate sandbox/release security checks.
- Playback output still uses the existing transparent Avalonia text overlay,
  so WebVTT position metadata is parsed but not rendered.

## Verification

`WebVttSubtitleSourceTests` exercises parse, overlap/boundaries, NOTE/STYLE,
markup normalization, forged/foreign track guards, cancellation, size limits,
source catalog selection and language fallback. The local container has no
.NET SDK, so managed CI must compile/run these tests before merge.

All changes are in desktop managed subtitle code and docs; no FFmpeg/MediaRuntime
C ABI, D3D11, WASAPI, BD-J or audio-master clock changes.
