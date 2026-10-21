# Generated subtitle timeline — initial AI/translation data boundary

This milestone introduces a **data plane**, not a speech recognizer,
translator, audio capture loop or remote AI client. It is deliberately
independent of FFmpeg, Avalonia and any vendor SDK.

## Flow

```text
Future audio tap (not implemented)
  → bounded audio frame transport (not implemented)
  → ASR/translation worker (not implemented)
  → media-time cue batch + generation + revision
  → GeneratedSubtitleTimeline.TryPublish(...)
  → immutable, bounded snapshot
  → ITextSubtitleTrack.At(media time) → existing Watch overlay
```

`GeneratedSubtitleTimeline` is in `Lapis.Contracts.Media`. It uses
media-relative `TimeSpan` timestamps, not wall-clock arrival time.
The UI reads immutable snapshots without locking or awaiting inference.
Writers validate and copy the complete replacement window before atomically
publishing it. Batches are capped at 512 cues and 4,096 characters per cue.
The consumer uses a binary search with prefix maximum end times to resolve
overlapping cues, favoring the latest cue start.

Call `BeginGeneration()` on **Open, Seek and Close**. It immediately
invalidates previous cues and returns a monotonically increasing token.
An ASR/translation result must supply that token and an increasing revision;
late or duplicate results are ignored, not shown on the new timeline.
A rolling window replaces the entire snapshot rather than appending
unbounded transcripts. The producer should retain its own bounded work queue.

`GeneratedTextSubtitleSource` is an **explicit opt-in** source adapter.
It can be registered in a session-scoped `TextSubtitleSourceCatalog` when
an AI producer and lifecycle are available. Its discovery and load methods
never launch an AI worker or open the network. Auto returns an empty track,
preserving the current sidecar-first policy. It checks the media path and
the caller-provided session-validity callback. It is **not** added to the
global default catalog, so no phantom AI subtitle choice is shown.

## Next integration milestones

1. Introduce a read-only native audio tap that cannot block or mutate the
   WASAPI render callback, with strict PCM format/clock contracts.
2. Use a bounded transport and worker to produce media-time ASR segments.
   Specify overload/drop behavior and resource limits before any SDK wiring.
3. Add a cancellable translator with language choice, bounded latency,
   stable partial/final revisions and generation-aware seek/close cleanup.
4. Register the source per playback session only after an actual producer
   exists; add user-visible opt-in and privacy controls.
5. Validate real-device synchronization, pause/resume and seek races.

This milestone does **not** implement AI translation, live audio capture,
PGS/BD-J bitmap subtitles, model downloads or any cloud requests.
