# Embedded text subtitle async timeline + optional Cue ABI

## Implemented increment

Native local-file SubRip / mov_text decoding now has a **separate, one-shot
background timeline**. The worker opens its own PlaybackMediaSession / FFmpeg
demuxer instead of borrowing the active audio/video packet router. It publishes
a complete owned snapshot only after decode and sorting finish. The UI /
application will query results via an optional C ABI, never call the blocking
FFmpeg decoder on the presentation or audio callback thread.

The existing ABI v2 and the read-only subtitle inventory extension are unchanged.

### Lifecycle

- Start: after a successful media Open, request a specific **actual container
  stream index**. Only declared text tracks with codec name "subrip" or
  "mov_text" are accepted. Bitmap PGS is refused.
- The task is limited to **local File locators**. BDMV, physical discs,
  remote URLs and streaming need explicit cancellation and source-access
  semantics first.
- Each task owns its decoder and source handle. It decodes at most
  **4,096 cues** and **4 MiB of cue text**, otherwise reports
  LIMIT_EXCEEDED with no partial result.
- On re-selection, Seek, Open, Close and session destruction, the old worker
  receives cancellation and is joined. A new Epoch cannot see the old cues.
- A completed timeline is immutable and allows bounded, read-only
  pagination; no repeated decoding per UI frame.

### Optional ABI v1 extension

- \`lapis_runtime_get_text_subtitle_cues_api_version()\` returns version 1.
- \`lapis_session_start_text_subtitle_load(session, epoch, stream_index)\`
  starts a separate worker without changing the playback transport or Epoch.
- \`lapis_session_read_text_subtitle_cues(...)\` is a **nonblocking**
  poll/page call (maximum 16 cues and 65,536 text bytes per call). It reports
  LOADING / READY / FAILED / LIMIT_EXCEEDED / CANCELLED.
- A zero-capacity call probes the total number of cues after READY; a
  further call uses an offset and a caller-owned \`lapis_text_subtitle_cue_info\`
  array plus a separate UTF-8 byte buffer. Cue text uses byte offsets/lengths,
  not a borrowed or NUL-terminated pointer.
- The fixed C record is 24 bytes: start/end signed nanoseconds, then
  32-bit \`text_offset\` / \`text_length\`.
- Too-small text buffers report \`LAPIS_E_BUFFER_TOO_SMALL\`, with the required
  byte count but no partial writes. Callers must still pass a non-null record
  buffer when nonzero cue capacity is requested.
- All calls share the command/Epoch lock with Open, Seek and Close.
  \`lapis_session_destroy\` retains its existing no-in-flight-calls contract.
- Consumers of older DLLs **must dynamically check the optional version export**
  before requesting this feature.

### Acceptance tests

\`async_embedded_text_subtitle_timeline\` decodes the existing real Matroska
SubRip fixture with a private source and verifies timestamps, two cues,
concurrent isolated video decoding, cancellation and invalid input.
\`async_embedded_text_subtitle_c_abi\` checks C record layout, version,
polling, pagination, insufficient buffers without partial write, stream
validation, retired Epoch on Seek and invalid-state after Close.

Both tests must compile and pass on the existing native CI runners before
merging. This environment has no FFmpeg development headers; local C++ builds
are not claimed.

## Deliberate limitations

- The extension **is not yet wired into .NET or Watch**, so embedded subtitles
  are not yet a playable user-facing feature.
- This is a finite, eager timeline with a hard memory budget, not incremental
  live streaming; excessively large subtitles return LIMIT_EXCEEDED.
- A source that blocks inside synchronous local disk/FFmpeg I/O can delay
  cancellation/join; source interrupt callbacks and bounded Close latency
  must be verified before permitting optical discs/network streams.
- Plain-text SubRip/mov_text only. ASS styling, PGS bitmap composition, BD-J,
  OCR and AI translation remain distinct milestones.
- Codec faults may be reported as FAILED or empty-ready depending on the
  underlying packet router's EOF/error visibility. Production telemetry should
  report precise source/decoder failures without exposing FFmpeg internals to UI.
