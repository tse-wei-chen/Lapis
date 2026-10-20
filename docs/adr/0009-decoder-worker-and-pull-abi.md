# ADR-0009 — Bounded decoder worker and owned CPU frame transfer

Status: **Accepted for M1c prototype** (2026-10-20). Supersedes no previous ADR.

## Context

M1b implemented a private FFmpeg adapter but Session could not consume real packets/frames. A synchronous `next()` exposed directly to the UI could block on disk/codec. A naïve unbounded decode-ahead queue risks huge memory and stale frames after seek. Cross-platform .NET requires ABI-safe lifetime rules that do not pass native FFmpeg objects.

## Decision

- `core::IMediaDecodeSource`: blocking local-file decoder interface returning **owned** RGBA8 and PCM S16 stereo buffers. FFmpeg remains in its adapter. Other decoders (hardware, Blu-ray) may implement this contract, but the CPU buffer DTO is not a replacement for the future GPU lease interface.
- `core::DecodePump`: exactly one decoder worker per session, decoder operations (`next` vs `seek`) protected by a codec lock, 8-item queue with **128 MiB byte budget**, no silently discarded frames on backpressure; seek increments Epoch, fences old output, clears the queue and any pending ABI frame. It may block until current synchronous `next()` finishes; cancellation is a separate milestone.
- C ABI **1.1** adds `bdmv_session_read_frame()` and `bdmv_decoded_frame_info`. It is nonblocking, transfers bytes into **caller-owned memory**, and uses a two-call buffer-size protocol. `BDMV_AGAIN` is not an error. `BDMV_BUFFER_TOO_SMALL` does not consume the frame. `BDMV_END_OF_STREAM` and `BDMV_DECODE_ERROR` are explicit. Existing ABI fields and previous enum values remain stable.
- `bdmv_frame_dump` is **offline output verification**, not an actual video presenter/audio sink. Synthetic finite source `test://frames` exists only with diagnostic builds.
- No presentation clock, audio master, resampling drift correction, pixel-format negotiation, YUV surfaces or HDR capability claims at this stage.

## Nonfunctional constraints / risks

- Worker and synchronous FFmpeg adapter are intended for *local test files* only. A blocked decoder operation can hold up Seek and Close. A later cancellation-aware asynchronous source contract must solve this before discs/streams.
- Test output is 8-bit RGBA + stereo S16 48k; not color-correct for HDR or bitstream output.
- Queue budget bounds queued storage, **not all temporary allocations inside FFmpeg**. 128 MiB maximum decoded RGBA frame size is enforced by the adapter DTO helper.
- The ABI is append-only: new calls require **minor version capability negotiation**, never a change to an existing structure's layout. Callers must check `struct_size` and ABI minor before invoking new entrypoints.
- Decode order is not the same as presentation order. A future PTS scheduler and audio hardware master will own A/V synchronization.

## Acceptance evidence

- GCC Release and Clang Debug with FFmpeg **OFF**: synthetic end-to-end C ABI, Epoch race, PPM+PCM dump and all prior tests pass.
- FFmpeg **ON**, real local MP4/MKV, MSVC, SteamOS, .NET 10, audio device / visible playback remain **unverified**.
