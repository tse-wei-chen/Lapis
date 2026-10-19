# ADR 0039: Epoch-safe chapter navigation via read-only resolution

- Status: Implemented (chapter seek control path; not a menu engine)
- Date: 2026-10-19

## Context

A clear BDMV source now exposes owned NavigationSnapshot metadata, but the
previous source Navigation controller deliberately returns NotReady when asked
to mutate title/chapter state. Calling bd_seek_chapter from WinForms or making
libbluray move its shared cursor during a live FFmpeg decode generation would
leave old packet/decoder/audio/render workers alive across a discontinuity.

## Decision

1. Add optional **navigation API v1** to the Native C ABI without changing
   playback ABI v2: `lapis_runtime_get_navigation_api_version()` and
   `lapis_session_resolve_chapter(session, chapter_id, expected_epoch,
   out_position)`. The call is read-only and operates only in Ready, Playing,
   or Paused states. It serializes with Open, Seek and Close at the same native
   session command gate and validates the current PlaybackEpoch.
2. Resolve IDs against the **source-selected Title only** and the Chapters
   capability bit. `ChapterInfo.id` is a source-defined identifier, never a
   vector position. Unknown, negative or cross-title entries are rejected.
   The extension returns an owned canonical nanosecond timestamp, with no
   libbluray pointers or mutable source handles.
3. The C# `IChapterNavigationRuntime` is an **optional** application port.
   The `SeekChapterCommand` runs wholly inside the serialized PlaybackSession
   actor: validate transport → resolve at current epoch → advance epoch →
   invoke existing `SeekCommand` path. NativePlaybackRuntime discovers the
   optional extension and fails explicitly on old binaries.
4. A failed/unsupported lookup must not advance PlaybackEpoch, stop workers,
   or fault an otherwise healthy session. No separate chapter-specific
   FFmpeg/D3D11/WASAPI worker path is created. The normal seek stop/join,
   demux seek, decoder flush, graph restart, clock rebase and epoch filters
   remain authoritative.
5. This is **time-based seeking** to the current title's chapter start. It
   neither invokes `INavigationController::execute()` nor claims full
   libbluray-native chapter hopping, seamless playlists, BD-J, angles or
   menu navigation. Disc-specific discontinuities which replace sources
   or change clip/decoder format require separate transactional handling.

## Verification

- Pure Native chapter resolution tests cover selected-title-only IDs,
  nonexistent chapter, mismatched Title, missing capability and negative
  timestamps. C ABI smoke covers optional API version, null output,
  unopened session, stale epoch, and files without chapter metadata.
- Managed tests validate lookup is observational (no implicit native Seek),
  actor serialized Chapter lookup → epoch-incrementing existing Seek,
  playback/pause state preservation, invalid chapter rejection without fault
  or epoch change, and optional extension version rejection.
- Default Windows CI covers C ABI, Native execution and WinForms lifecycle.
  The already verified opt-in libbluray build covers adapter compilation,
  but not actual chapter navigation on a physical/unprotected Blu-ray.

## Next steps

Expose a read-only current-title Chapter catalog to the application and
build a chapter picker only when the source advertises the capability.
Validate timestamp/clip transitions on actual unprotected BDMV fixtures.
Implement Title/Playlist changes as whole graph/source replacement
transactions, not by calling mutable libbluray navigation during playback.
