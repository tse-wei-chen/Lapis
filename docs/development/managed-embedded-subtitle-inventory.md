# Managed embedded subtitle inventory (metadata only)

## Purpose

The native FFmpeg source inventory from PR #47 is accessible to the managed
player runtime **without exposing native FFmpeg objects to application code**.

The new optional Application port is
`IEmbeddedSubtitleTrackInventoryRuntime.ReadEmbeddedSubtitleTracksAsync`.
It returns owned `EmbeddedSubtitleTrack` records:

- `StreamIndex`: actual container stream index (not subtitle-list position)
- `Format`: `Text`, `Bitmap` or `Unknown`
- `LanguageTag`: container-provided language, possibly empty
- `CodecName`: FFmpeg codec name (for diagnostics and future compatibility)

No Watch UI entries are added yet: discovery alone is **not** proof of decode
support, especially for PGS/image subtitles.

## Interop and compatibility rules

- The base `Lapis.MediaRuntime` ABI remains version 2.
- Managed code calls the optional version function and accepts only subtitle
  inventory extension version 1, otherwise raises `NotSupportedException`.
  The extension is not queried in the native runtime constructor.
- Missing DLL exports (`EntryPointNotFoundException`) are treated as a
  compatible older native runtime, not a reason to reject the base player.
- Read requires an already-opened matching `PlaybackSessionId`.
  It never allocates a new native Session or changes transport / playback
  Epoch.
- A count-only probe is followed by a bounded second read into an owned
  managed array; both pass the caller's playback Epoch to native code.
- Limit is 64 tracks; count overflow, buffer-size mismatch and malformed
  non-NUL-terminated UTF-8 fields raise an explicit error.
- Fixed 72-byte layout: uint32 stream index, uint32 format,
  24-byte language and 40-byte codec string. Native P/Invoke memory uses
  fixed inline buffers with no string pointers or GC lifetime leakage.
- The caller must still respect application cancellation and seek/close
  generation validity when handling the result. The native ABI itself
  serializes metadata reads against Open / Seek / Close.

## Tests / next feature

`NativePlaybackRuntimeTests` verifies layout offsets, text/bitmap/unknown
mapping, original stream index, missing or incompatible extension, no
Session creation before Open, empty inventory, stale epoch, invalid state,
cancelled calls, malicious count and missing string termination.

This change **does not** decode subtitle packets or display text. The next
milestones are stream-selectable subtitle packet routing and per-Epoch
text decoding, then a concrete `ITextSubtitleSource` for playable embedded
text tracks. Bitmap PGS still needs a separate surface/timeline pipeline.
