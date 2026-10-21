# Embedded subtitle stream inventory — initial native milestone

## Actual implementation

The FFmpeg demuxer now inspects **existing container stream descriptors** once
at Open. The owned `SubtitleStreamInfo` snapshot includes:

- the **actual container stream index** (not list ordinal)
- the FFmpeg codec name
- the container's language metadata, when present
- a `Text / Bitmap / Unknown` classification from FFmpeg codec properties

`PlaybackMediaSession` retains this immutable snapshot across Seek. It is
cleared on a new Open / teardown; none of the returned values borrow AVStream,
AVCodecParameters or FFmpeg metadata pointers.

## C ABI extension (base ABI v2 remains unchanged)

- `lapis_runtime_get_subtitle_tracks_api_version()` → 1
- `lapis_session_list_subtitle_tracks(session, epoch, records, capacity, out_count)`
- fixed 72-byte `lapis_subtitle_track_info` (stream index, kind, language and
  codec ASCII strings in NUL-terminated UTF-8-compatible arrays)
- query with `records=nullptr, capacity=0` to obtain the required count;
  insufficient nonzero capacity returns `LAPIS_E_BUFFER_TOO_SMALL`, with no
  partial writes
- stale epoch, invalid state, null pointer and excess-capacity handling
- lock acquisition serialized against Open/Seek/Close; no subtitle decoder
  or worker threads are started by the metadata query
- maximum 64 tracks reported

**Compatibility:** This is an optional export. Consumers targeting an older
DLL must discover the version function using GetProcAddress/dlsym before
importing or invoking the extension. No existing ABI v2 structs/functions
change their size or ordering.

## Tests

`embedded_subtitle_track_inventory` uses in-memory FFmpeg AVFormatContext
fixtures to verify subtitle stream classification, language metadata and
non-contiguous subtitle stream indices. Its C ABI probe opens the existing
Y4M video fixture, verifies zero subtitle tracks and tests stale epochs,
closed-session behavior, null arguments and descriptor layout. It also checks
the version export.

## Not implemented / next integration gate

This PR **does not decode subtitle packets** or expose embedded subtitles in
the Watch selector. It is deliberately not a functional PGS renderer or a
claim that each reported codec is supported.

Next work: managed optional-export negotiation and owned descriptor mapping;
subtitle packet routing and text decoding scoped to epoch + stream index;
normalized cue timing and seek flush; bitmap PGS requires independent
overlay surfaces and composition. Do not connect a subtitle descriptor to
`ITextSubtitleSource.LoadAsync` until a real timed text decoder is available.
