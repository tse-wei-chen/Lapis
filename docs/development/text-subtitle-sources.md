# Text subtitle source boundary — Lapis

## Scope

This milestone migrates the existing SRT picker to an extensible **timed-text**
source contract. It does not implement in-file text subtitle demuxing, AI speech
recognition/translation or bitmap subtitles.

```text
Lapis.Contracts.Media
  ITextSubtitleSource ── DiscoverAsync(media, ct)
                      ├── LoadAsync(media, descriptor, ct)
                      └── LoadPreferredAsync(media, culture, ct)
  ITextSubtitleTrack  ── At(media_time)
  TextSubtitleDescriptor(SourceId, TrackId, DisplayName, LanguageTag, Kind)

Lapis.Desktop.Avalonia.Services
  TextSubtitleSourceCatalog  ── source-qualified routing; bounded discovery
  SidecarSrtTextSubtitleSource ── existing SidecarSubtitleTrack parser

Lapis.Desktop.Avalonia.Views
  MainWindow ── preferred track through the catalog
  WatchChromeWindow ── catalog discovery / selected track / disable / cancel
```

The contracts are in **Lapis.Contracts**, with no Avalonia, FFmpeg, Windows
handle or native C ABI dependencies. The first registered source is the Auto
policy, currently only `local-srt`. Auto never launches a generated/AI provider
without deliberate future integration.

## Lifecycle and invariants

- `SourceId` is stable per provider. `TrackId` is opaque and only interpreted by
  that provider. Multiple providers can reuse the same TrackId without collision.
- Discovery returns at most 64 descriptors across providers; the SRT source
  separately limits itself to 32 sibling candidates.
- Per-provider descriptors must identify their declaring source; unexpected
  owners, blank IDs and duplicate provider IDs are rejected.
- Sidecar selection validates the media directory and filename before reading;
  the 2 MiB SRT read cap, 5,000 cue cap, and existing overlap-safe cue lookup
  remain in effect. Symlink policy and sandboxed file grants remain follow-up
  security work.
- UI requests are canceled when the selection, Watch session or window closes.
  Watch additionally uses generation checks against stale async completions.
- The same catalog instance is passed from MainWindow to WatchChromeWindow;
  optional source injection is available without forking the UI.
- The `ITextSubtitleTrack.At(position)` clock input is media playback time,
  not machine/wall-clock time. Live incremental generated timelines require a
  separate immutable snapshot/notification protocol, not UI polling of an AI API.

## Future adaptation

1. Implement an embedded **text** subtitle source that maps a media track ID to
   an owned timed-text timeline using existing native stream metadata.
2. Add an AI text subtitle source behind a nonblocking IPC observer; never
   await ASR/translation from the render/audio callback.
3. Define a different bitmap/overlay surface/timeline for PGS and BD-J;
   do not store PGS images in `string? At(TimeSpan)`.
4. Extend selection policy to be user-configurable instead of first provider.
5. Add hardware/UI tests for transparent overlay, changing tracks while paused,
   seek races and session closure on Windows/SteamOS.

## Test coverage

`TextSubtitleSourceCatalogTests` verifies same track IDs across different
sources, auto-source precedence, rejected source identity, pre-cancellation,
real local SRT discovery/load and path guards. Existing `SidecarSubtitleTrackTests`
continues to cover SRT parsing, overlap, culture fallback, explicit selection
and bounded local discovery.

CI still must compile/run the managed tests. No native audio/video subsystem
is changed by this PR.
