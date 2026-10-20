# M1a — Media foundations implementation report

**2026-10-20 | Status: tested partial implementation — not video playback**

The new files continue the v0.2 architecture while keeping the stable C ABI unchanged.

## Implemented

| Subsystem | Code | Contract |
|---|---|---|
| Local byte source | `native/MediaRuntime/src/adapters/file/file_source.*` | UTF-8 path/URI, percent decoding, concurrent bounded reads by caller-sized span, absolute seek offsets and EOF handling |
| Monotonic playback clock | `native/MediaRuntime/src/core/playback_clock.hpp` | deterministic play/pause/seek and saturating arithmetic, time source injected by caller |
| Rational timestamps | `native/MediaRuntime/src/core/media_time.hpp` | 90kHz / 48kHz source ticks converted to signed ns, checked overflow/error |
| Frame/packet queue building block | `native/MediaRuntime/src/core/epoch_queue.hpp` | bounded queue, stale Epoch + SessionId rejection, optional drop-oldest |
| Native file diagnostic | `tests/native/file_probe.cpp` | exercises the same file adapter against actual MP4 and MKV fixtures, without claiming parsing |
| Media fixture diagnostic | `tools/ffmpeg_media_diagnostic.py` | out-of-process FFmpeg/ffprobe decodes 3 RGB24 frames and 0.25 sec of PCM from actual MP4 and MKV |

## Local commands

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBDMV_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 tools/ffmpeg_media_diagnostic.py --self-test --native-probe build/tests/native/bdmv_file_probe
```

`--input /path/to/clip.mp4` may replace `--self-test` for an existing local media file.

## What's missing for M1 playback

- A proper in-process FFmpeg `DemuxAdapter` using `libavformat` + `libavcodec`, with decoded frame/packet ownership and hardware-independent data types. The diagnostic script is **not a substitute**.
- Demux track map and `AV_NOPTS_VALUE`, duplicate PTS, B-frames, edit lists, seeking to keyframes, decoder flush, timestamp discontinuities and session Epoch propagation.
- Frame color conversion or frame-to-presenter handoff plus concrete Windows/Linux video output; PCM resampler, WASAPI / PipeWire sink, device clocks and A/V synchronization.
- Command mailbox / cancellation / bounded async reader / source errors to snapshot / SafeHandle lifetime, multi-track, subtitles, BD source, HDR and SteamOS real-device validation.

`FileSource` is intentionally not yet connected to `bdmv_session_open_uri`; opening ordinary files via the C ABI remains `BDMV_UNSUPPORTED` and `file_source` capability stays 0. This prevents users from mistaking readable bytes for decoded and playable media.
