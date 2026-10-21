# Embedded text subtitle decoder — first native slice

## What is implemented

This increment provides a **real opt-in decode path** for FFmpeg SubRip
(\`AV_CODEC_ID_SUBRIP\`) and MP4 \`mov_text\` (\`AV_CODEC_ID_MOV_TEXT\`).

- \`FfmpegDemuxer::text_subtitle_stream(index)\` validates the exact stream
  index and codec before passing codec parameters to the FFmpeg adapter.
- \`MediaDecodeSelection\` gains an optional subtitle stream index. Existing
  video/audio-only call sites leave it unset, so **no subtitle queue is added
  unless there is an active consumer**. A selected subtitle shares the existing
  bounded per-stream packet router with video/audio.
- \`FfmpegTextSubtitleDecoder\` owns its AVCodecContext, produces owned
  \`TextSubtitleCue { start, end, text }\` records and supports flush/recreate
  across decoding generations.
- Text is taken from FFmpeg \`AVSubtitleRect::text\` or parsed from the ASS
  event payload that FFmpeg emits for SubRip; style override tags and
  HTML-like tags are flattened to plain text. The output is limited to
  4096 bytes per cue; no bitmap subtitle packets are accepted.
- PTS is mapped from FFmpeg's AV_TIME_BASE or packet time base into
  nanosecond \`MediaTime\`, accounting for container timestamp origin and
  subtitle display offsets; zero/invalid durations are not published.
- Seek and Close already stop the original packet router; the new decoder
  is discarded with that generation. A new reader cannot receive old queued
  subtitles from an earlier seek.

## Acceptance test

\`tests/assets/test_embedded_subtitles.mkv\` is a 987-byte **real Matroska**
fixture with MPEG-4 video (stream 0), English SubRip (stream 1), and two
timestamped cues:

- 1.000–3.000 s: "Hello embedded text"
- 3.000–5.000 s: "Second cue"

The test checks actual stream selection, timestamps, text, EOF, backward
seek/re-open and mixed video+subtitle routing. It deliberately does not use
an external \`ffmpeg\` subprocess during test execution.

## Not yet production playback

The \`PlaybackMediaSession::decode_next_text_subtitle_cue()\` consumer is a
native diagnostic/test seam. This PR **does not add a public C ABI cue read,
a C# subtitle source, a background UI-safe subtitle worker, or Watch embedded
subtitle selection**. Merely listing the stream does not make PGS or ASS fully
playable. The decoder only supports a conservative plain-text subset of
SubRip/mov_text and discards ASS styling; packet cancellation/seek and source
time alignment require wider real-content validation.

Next: turn decoded cue ownership into a bounded asynchronous subtitle
timeline, expose an optional epoch-fenced C ABI for text cues, and adapt the
managed \`ITextSubtitleSource\` only after these gates pass.

**Compatibility:** No public ABI v2 layout or optional metadata ABI v1 layout
is changed by this feature. \`master\` is not a development target.
