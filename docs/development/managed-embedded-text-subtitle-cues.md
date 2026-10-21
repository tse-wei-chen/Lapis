# Managed embedded text subtitle cue interop

## Scope

This milestone makes the **real native SubRip/mov_text cue timeline** from
PR #50 accessible to application code with owned .NET records. It does not
yet automatically display embedded subtitles in Watch.

The managed extension is optional and does **not** change the base
\`IPlaybackRuntime\`/Native ABI v2. It is separate from the already-merged
\`IEmbeddedSubtitleTrackInventoryRuntime\`.

## Application contract

\`IEmbeddedTextSubtitleCueRuntime\` offers:

- \`StartEmbeddedTextSubtitleLoadAsync(streamIndex, context, ct)\`
  to request decoding of an actual container stream index.
- \`ReadEmbeddedTextSubtitleCuesAsync(startIndex, context, ct)\`
  to poll/obtain up to 16 owned text cues from a completed native timeline.
- \`EmbeddedTextSubtitleCuePage\`: Loading / Ready / Failed /
  LimitExceeded / Cancelled, completed timeline count, immutable cue list.
- \`EmbeddedTextSubtitleCue\`: source media start/end nanoseconds and
  ordinary managed UTF-8-decoded text, not a borrowed native pointer.

Every call is bound to an already-created matching playback SessionId and
the caller's current Epoch. Neither operation creates a native playback
session, seeks, changes transport, nor borrows the live A/V packet router.
A Seek, new Open or Close invalidates the native worker; the next read must
use the new Epoch and launch a new load as appropriate.

## Native compatibility and validation

- Optional version export \`lapis_runtime_get_text_subtitle_cues_api_version\`
  is negotiated only when the feature is requested; version 1 required.
- If an older DLL lacks the export, normal playback still works; invoking
  this optional API raises \`NotSupportedException\`.
- P/Invoke buffers are pinned for the duration of each call. A 24-byte
  blittable cue struct mirrors the C ABI's signed start/end nanoseconds
  and unsigned UTF-8 byte offset/length.
- Each read requests at most **16 cues / 65,536 bytes**. Native decoding
  itself caps at **4,096 cues / 4 MiB**.
- Validate returned count/total/required bytes, per-cue intervals, offsets,
  positive text size, and strict UTF-8. Unknown native load states and
  nonempty results reported while Loading/Failed are rejected.
- Calls run off the UI thread and serialize through the existing managed
  native-session gate, including cancellation checks before publishing.
- All strings are copied and decoded; neither the native buffers nor the
  mutable native cue array escape into Contracts.

## Test cases

\`NativePlaybackRuntimeTests\` checks record layout and offsets, normal
CJK/emoji UTF-8 decode, exact nanosecond timestamps, session isolation,
cancellation, old DLLs and incorrect extension versions, all native
loading states, invalid native counts, offset/time/UTF-8 corruption,
and stale Epoch/closed-session errors.

## Next integration gate

The Watch overlay requires a per-Session/epoch adapter to
\`ITextSubtitleTrack\` and a seek-generation lifecycle. The UI must not
start a loader from the rendering tick, poll P/Invoke on the Avalonia
dispatcher, show bitmap PGS as a text subtitle, or accept an old
generation after the user switches track or leaves Watch.

An embedded source is not registered as a playable Watch entry until
the application playback owner exposes the active RuntimeCommandContext
safely and the above generation checks are exercised.
