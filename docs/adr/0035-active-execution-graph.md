# ADR 0035: Compile a validated graph at every playback generation boundary

- Status: Accepted
- Date: 2026-10-19

## Context

The file source and FFmpeg demuxer report a discovery GraphPlan containing
Source -> Demuxer -> selected VideoDecoder/AudioDecoder nodes, while
PlaybackExecution previously selected its actual video/audio worker chains using
separate media booleans. The discovery topology therefore did not validate
which branches the runtime could activate. Future subtitles, AI taps and
additional output adapters would magnify that divergence.

## Decision

1. Keep the discovery graph independent of output availability. It describes
   which decoders can be opened, not which consumers have been configured.
2. Add the pure native `compile_execution_graph` step. It rejects empty
   selections and decoders that lack a typed Source -> Demuxer -> Decoder path.
3. Materialize only selected branches in a new immutable GraphPlan:
   - Source -> Demuxer -> VideoDecoder -> VideoProcessor -> VideoScheduler -> VideoSink
   - Source -> Demuxer -> AudioDecoder -> AudioProcessor -> AudioSink
4. PlaybackExecution selects branches from the compiled plan's terminal sinks,
   not directly from raw source-discovery booleans. Audio remains disabled when
   the output provider/processor factory is absent.
5. Publish the active plan only after the selected worker generation starts.
   Retire it after downstream-to-upstream worker teardown. Seek compiles a fresh
   plan for its next epoch. Closed/failed-start generations expose no active plan.
6. Diagnostics consumers receive a value copy under the execution control
   mutex; no borrowed graph pointer may outlive a Seek/Close.
7. Preserve the existing C ABI, FFmpeg demux ownership, bounded queues,
   device ownership, and single authoritative PlaybackClock. No dynamic
   node mutation or GPU/AI code is introduced.

## Verification

- Pure graph-plan regression covers video-only, audio-only, combined paths,
  branch omission, missing/disconnected/mislabeled decoders and empty selection.
- Real PlaybackExecution regression asserts active video/audio sink plans
  on Start, plan regeneration after Seek, and plan retirement after Close.
- Existing full Windows Native CTest remains the compatibility gate.

## Limitations and follow-ups

This is a **validated branch activation plan**, not yet a generic node factory
or arbitrary plugin execution scheduler. Concrete workers remain statically
composed in PlaybackExecution, with known bounded startup/shutdown order.
Future GraphRuntime work must bind typed nodes to factory-owned runtime
instances, add resource preparation/rollback and prove atomic replacement,
without introducing unrestricted in-place mutation. Blu-ray navigation and
AI Tap are separate future graph branches, not placeholders exposed as
working functionality.
