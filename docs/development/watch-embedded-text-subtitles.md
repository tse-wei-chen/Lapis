# Watch embedded text subtitles — first end-to-end desktop integration

## Shipped user-visible behavior

With a compatible native runtime (optional text Cue API v1), Watch shows
embedded **SubRip** and **mov_text** tracks in the *same* subtitle selector as
local SRT and WebVTT for ordinary local media files.

- The existing \`TextSubtitleSourceCatalog\` remains the single Watch chooser.
- **Auto** continues preferring local SRT; no embedded subtitle decoder starts
  until the user explicitly chooses an embedded track.
- Track display names include the container's language tag (when present) and
  codec. The opaque TrackId is the actual container stream index.
- Native metadata tracks that are PGS/bitmap, ASS or unknown codecs are NOT
  exposed as playable text choices.
- Older DLLs that can enumerate tracks but lack the optional Cue decoder API
  show **no embedded choices**. SRT/WebVTT and regular playback still work.
- After choosing an embedded text track, the native worker opens a separate
  local-file demuxer. Managed code polls bounded Cue pages off the UI thread,
  converts nanoseconds into Timespan ticks, and builds the same overlap-safe
  immutable timeline index as SRT and WebVTT.
- Watch's existing transparent subtitle overlay renders via
  \`ITextSubtitleTrack.At(playbackPosition)\`, not a new UI layer.

## Session/epoch correctness

The component boundaries deliberately remain distinct:

\`WatchChromeWindow\` → \`EmbeddedTextSubtitleSource\` →
\`IEmbeddedSubtitlePlaybackService\` → \`NativePlayerPlaybackService\` →
\`DesktopPlaybackHost\` → optional Application runtime ports →
\`NativePlaybackRuntime\` → native Cue ABI.

The host serializes each inventory, start and page read against the same
command lock as Open/Seek/Close and rejects requests whose captured
\`PlaybackSnapshot.SessionId\` or \`Epoch\` differs from the current one.

The Watch source also validates the media path, the session identity and
stream IDs previously discovered by that source. A loaded
\`EpochGuardedTrack\` returns **no text** immediately on an epoch change,
even before asynchronous replacement finishes. Watch detects a new Epoch
while an embedded track is selected, cancels the old load, clears the overlay
and reloads the selected stream once for the new generation.

The existing selection-generation/cancellation checks still prevent a slow
old track or a closed Watch window from republishing captions after the
user switches tracks, chooses Off or returns to the library.

## Tests

- Desktop hosting tests exercise genuine PlaybackSession actor Open,
  Seek/Close lifecycle and prove a subtitle load cannot be overtaken by
  another host command.
- Desktop subtitle source tests cover codec/bitmap exclusion, older DLL
  capability gating, Unicode/name metadata, empty/corrupt pages, cancellation,
  media-path and stream-ID identity, overlapping cue lookup, seek invalidation
  and reload, and Auto policy.
- Managed CI compiles/tests the desktop modules. An installed Native DLL is
  not required for the fakes; the real native decode/ABI was tested in #49–#50.

## Intentionally unsupported

Only files (not BDMV/optical/remote media) have a fully tested independent
native subtitle source. PGS/BD-J bitmap overlays and ASS styling remain
separate feature work. If decoding a large local file takes too long, the
initial implementation has a one-minute UI load timeout and shows no
partial/possibly stale text. A native worker may finish its bounded load
after a UI selection was cancelled because the existing optional ABI does
not yet expose explicit \`cancel text load\`. Seek/Open/Close still retire it,
and later stream selection replaces its worker. A future native API could
retire it immediately on Off/SRT, after cancellation and bounded Close
behavior are tested.
