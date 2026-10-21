# Embedded subtitle request-scoped native cancellation

## Why this change

PR #52 made embedded SubRip/mov_text subtitle text selectable in Watch,
but selecting Off or another language only invalidated the *managed* result.
The native worker could continue decoding and retain a completed cue timeline
until a later Seek, Open, Close or track replacement.

This feature adds **cooperative request-scoped cancellation** end-to-end.

## Optional Native C ABI (backward-compatible)

The existing base ABI v2, text-cue API v1 and old
\`lapis_session_start_text_subtitle_load\` remain unchanged.

New independently negotiated exports:

- \`lapis_runtime_get_text_subtitle_cancel_api_version()\` → 1
- \`lapis_session_start_text_subtitle_load_with_id(session, epoch,
  stream_index, request_id)\`
- \`lapis_session_cancel_text_subtitle_load(session, epoch, request_id)\`

\`request_id\` is an opaque **nonzero 64-bit selection identity**, unique for
each Watch selection or Seek reload, even when the stream index is the same.
The native pipeline stores the ID beside the single worker.

Start still validates the container's real stream index, text codec,
local-file restriction and playback Epoch exactly like the v1 start.
Cancellation uses the same command mutex as Open/Seek/Close and only stops
the worker if both Epoch and request ID match. An old cancellation cannot
stop a newer worker, and repeating a cancel is harmless.

\`AsyncTextSubtitleTimeline::cancel()\` requests \`jthread\` stop and clears
published cues immediately without waiting for the decoding thread to
join. The worker cooperatively checks stop tokens between decoder packets;
**blocking local disk or FFmpeg I/O may delay actual worker exit**.
Start/replacement and Seek/Close still synchronously join retired worker
state as required by the existing lifecycle.

A retired Epoch returns \`LAPIS_E_STALE_EPOCH\`. Closed sessions are
invalid. Null Session and zero request ID are rejected.

## Managed and UI integration

A separate optional Application port
\`IEmbeddedTextSubtitleCancellationRuntime\` preserves the legacy Cue v1
contract. \`NativePlaybackRuntime\` negotiates the new extension only
when requested. An older DLL still displays embedded text via Cue API v1,
but without early native cancellation.

\`DesktopPlaybackHost\` serializes start/cancel through the same operation
gate as Open/Seek/Close; every call validates the captured SessionId/Epoch.
The optional \`IEmbeddedSubtitlePlaybackService\` exposes the request-aware
operations to desktop without leaking native pointers or P/Invoke.

\`EmbeddedTextSubtitleSource\` issues a unique request ID per load. If
the load is cancelled, times out, fails or becomes stale, it sends
best-effort native cancellation with an *independent noncancelled token*.
On a successful load, it transfers ownership to the immutable
\`EpochGuardedTrack\` which implements \`IAsyncDisposable\`. The Watch
overlay asynchronously disposes the old track when switching to Off,
SRT/WebVTT, another embedded stream, seeking or closing Watch.

The old track hides immediately after Epoch change; the host rejects its
obsolete cancellation if Seek already retired that native generation.
An obsolete request-ID cancel cannot affect a newly started worker at
the same Epoch.

## Verification

- Native C ABI fixture checks version export, old v1 entry points,
  request-ID validation, idempotent cancellation, READY→CANCELLED
  snapshot, delayed A cancellation after B's start, Seek stale epochs and
  Close invalid state.
- Managed Native fake checks missing/incompatible optional exports,
  cancellation token, active-session binding, legacy decoder compatibility
  and request-ID forwarding.
- Desktop Hosting fake checks serialization against Seek/Close and
  captured Epoch, including stale request cancellation.
- Desktop subtitle source fake checks idempotent disposal, same-stream
  re-selection, cancellation during loading, error cleanup and fallback
  when an older DLL lacks cancellation exports.

## Limitations

Cancellation is **cooperative** and currently limited to local file media.
No promise of instant interruption during blocking FFmpeg I/O is made.
PGS/BD-J bitmap subtitles, ASS styling, live streams and AI translation
remain outside this change.
